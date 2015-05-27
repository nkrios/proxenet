#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>

#ifdef __LINUX__
#include <alloca.h>
#endif

#ifdef __FREEBSD__
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "control-server.h"
#include "core.h"
#include "socket.h"
#include "utils.h"
#include "plugin.h"

#define BUFSIZE 2048


static struct command_t known_commands[] = {
	{ "quit", 	 0, &quit_cmd,     "Make "PROGNAME" leave kindly" },
	{ "help", 	 0, &help_cmd,     "Show this menu" },
	{ "pause", 	 0, &pause_cmd,    "Toggle pause" },
	{ "info", 	 0, &info_cmd,     "Display information about environment" },
	{ "verbose", 	 1, &verbose_cmd,  "Get/Set verbose level"},
	{ "reload", 	 0, &reload_cmd,   "Reload the plugins" },
	{ "threads", 	 0, &threads_cmd,  "Show info about threads" },
	{ "plugin", 	 1, &plugin_cmd,   "Get/Set info about plugin"},
        { "config", 	 1, &config_cmd,   "Edit configuration at runtime"},

	{ NULL, 0, NULL, NULL}
};


/**
 * Send detailed information to socket
 *
 * @param fd the socket to send data to
 */
static void _send_threads_info(sock_t fd)
{
        char msg[BUFSIZE] = {0,};
        int i, n;
        bool first_item = true;

        n = snprintf(msg, sizeof(msg),
                     "{\"Threads\":{"
                     "\"Max threads\": %d,"
                     "\"Running threads\": %d,",
                     cfg->nb_threads, get_active_threads_size());
        proxenet_write(fd, msg, n);

        proxenet_write(fd, "\"Details\": {", 12);
        for (i=0; i < cfg->nb_threads; i++) {
                if (!is_thread_active(i)) continue;
                if (first_item) first_item=false;
                else proxenet_write(fd, ",", 1);
                memset(msg, 0, sizeof(msg));
                n = snprintf(msg, sizeof(msg), "\"Thread-%d\": %lu", i, threads[i]);
                proxenet_write(fd, (void*)msg, n);
        }
        proxenet_write(fd, "}", 1);

        proxenet_write(fd, "}}", 2);
        return;
}


/**
 * Enumerate all plugins loaded
 */
static int plugin_cmd_list_json(sock_t fd, bool only_loaded)
{
        plugin_t *p;
        bool first_iter=true;
        char msg[BUFSIZE] = {0, };
        int n;

        for (p=plugins_list; p; p=p->next) {
                if (only_loaded && p->state!=INACTIVE)
                        continue;

                if(first_iter) first_iter=false;
                else proxenet_write(fd, ",", 1);

                memset(msg, 0, sizeof(msg));
                n = snprintf(msg, sizeof(msg),
                             "\"%s\": {"
                             "\"id\": %d,"
                             "\"priority\": %d,"
                             "\"type\": \"%s [0x%x]\","
                             "\"state\": \"%sACTIVE\"}",
                             p->name,
                             p->id,
                             p->priority,
                             supported_plugins_str[p->type],
                             p->type,
                             (p->state==ACTIVE?"":"IN"));
                proxenet_write(fd, msg, n);
        }

        return 0;
}


/**
 * This command will try to kill gracefully the running process of proxenet
 */
void quit_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char *msg = "Leaving gracefully\n";

        /* happy compiler means karma++ */
        (void) options;
        (void) nb_options;

        proxenet_write(fd, (void*)msg, strlen(msg));
        proxy_state = INACTIVE;
        return;
}


/**
 * The famous help menu
 */
void help_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        struct command_t *cmd;
        char *msg;
        unsigned int msglen = 20 + 80 + 3;

        (void) options;
        (void) nb_options;

        msg = "Command list:\n";
        proxenet_write(fd, (void*)msg, strlen(msg));

        for (cmd=known_commands; cmd && cmd->name; cmd++) {
                msg = alloca(msglen+1);
                proxenet_xzero(msg, msglen+1);
                snprintf(msg, msglen+1, "%-15s\t%s\n", cmd->name, cmd->desc);
                proxenet_write(fd, (void*)msg, strlen(msg));
        }

        return;
}


/**
 * (De)-Activate pause mode (suspend and block interception)
 */
void pause_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char *msg;

        (void) options;
        (void) nb_options;

        if (proxy_state==SLEEPING) {
                msg = "sleep-mode -> 0\n";
                proxy_state = ACTIVE;
        } else {
                msg = "sleep-mode -> 1\n";
                proxy_state = SLEEPING;
        }

        xlog(LOG_INFO, "%s", msg);
        proxenet_write(fd, (void*)msg, strlen(msg));
        return;
}


/**
 * Get information about proxenet state.
 */
void info_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char msg[BUFSIZE] = {0, };
        int i, n;

        (void) options;
        (void) nb_options;

        /* generic info  */
        n = snprintf(msg, sizeof(msg),
                     "{\"info\":"
                     " {\"Configuration\":{"
                     "  \"Listening interface\": \"%s/%s\","
                     "  \"Supported IP version\": \"%s\","
                     "  \"Logging file\": \"%s\","
                     "  \"SSL private key\": \"%s\","
                     "  \"SSL certificate\": \"%s\","
                     "  \"Proxy\": \"%s [%s]\","
                     "  \"Plugins directory\": \"%s\","
                     "  \"Autoloading plugins directory\": \"%s\","
                     "  \"Number of requests treated\":\"%lu\""
                     "  },",
                     cfg->iface, cfg->port,
                     (cfg->ip_version==AF_INET)? "IPv4": (cfg->ip_version==AF_INET6)?"IPv6": "ANY",
                     (cfg->logfile)?cfg->logfile:"stdout",
                     cfg->keyfile,
                     cfg->cafile,
                     cfg->proxy.host ? cfg->proxy.host : "None",
                     cfg->proxy.host ? cfg->proxy.port : "direct",
                     cfg->plugins_path,
                     cfg->autoload_path,
                     (request_id-1)
                    );
        proxenet_write(fd, (void*)msg, n);

        /* threads info  */
        proxenet_write(fd, "\"Threads\": {", 12);
        n = snprintf(msg, sizeof(msg), "\"Running/Max threads\": \"%d/%d\",",
                     get_active_threads_size(), cfg->nb_threads);
        proxenet_write(fd, (void*)msg, n);

        proxenet_write(fd, "\"Details\": {", 12);
        for (i=0; i < cfg->nb_threads; i++) {
                if (!is_thread_active(i)) continue;
                memset(msg, 0, sizeof(msg));
                n = snprintf(msg, sizeof(msg), "\"Thread-%d\": \"%lu\",", i, threads[i]);
                proxenet_write(fd, (void*)msg, n);
        }
        proxenet_write(fd, "}},", 3);

        /* plugins info */
        proxenet_write(fd, "\"Plugins\": {", 12);
        if (proxenet_plugin_list_size()) {
                plugin_cmd_list_json(fd, false);
        } else {
                proxenet_write(fd, "\"info\": \"No plugin loaded\"", 26);
        }
        proxenet_write(fd, "}", 1);

        proxenet_write(fd, "}}", 2);
        return;
}


/**
 * Increase/Decrease verbosity of proxenet (useful for logging/debugging)
 */
void verbose_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char msg[BUFSIZE] = {0, };
        char *ptr;
        int n, level;

        (void) options;
        (void) nb_options;

        ptr = strtok(options, " \n");
        if (!ptr){
                n = snprintf(msg, BUFSIZE, "{\"verbose\": \"current level is %d\"}",
                             cfg->verbose);
                proxenet_write(fd, (void*)msg, n);
                return;
        }

        if ( (level=atoi(ptr)) > 0 && level<MAX_VERBOSE_LEVEL){
                cfg->verbose = level;
                n = snprintf(msg, BUFSIZE, "{\"verbose\": \"level is set to %d\"}",
                             cfg->verbose);
        }
        else if (strcmp(ptr, "inc")==0 && cfg->verbose<MAX_VERBOSE_LEVEL)
                n = snprintf(msg, BUFSIZE, "{\"verbose\": \"level is now %d\"}", ++cfg->verbose);
        else if (strcmp(ptr, "dec")==0 && cfg->verbose>0)
                n = snprintf(msg, BUFSIZE, "{\"verbose\": \"level is now %d\"}", --cfg->verbose);
        else
                n = snprintf(msg, BUFSIZE, "{\"error\": \"Invalid action\"}");

        proxenet_write(fd, (void*)msg, n);

        return;
}


/**
 * Reload proxenet
 */
void reload_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char *msg;

        (void) options;
        (void) nb_options;

        if (get_active_threads_size() > 0) {
                msg = "Threads still active, cannot reload\n";
                proxenet_write(fd, (void*)msg, strlen(msg));
                return;
        }

        proxy_state = SLEEPING;

        proxenet_destroy_plugins_vm();
        proxenet_free_all_plugins();

        if( proxenet_initialize_plugins_list() < 0) {
                msg = "Failed to reinitilize plugins\n";
                proxenet_write(fd, (void*)msg, strlen(msg));
                proxy_state = INACTIVE;
                return;
        }

        proxenet_initialize_plugins();

        proxy_state = ACTIVE;

        msg = "Plugins list successfully reloaded\n";
        proxenet_write(fd, (void*)msg, strlen(msg));

        return;
}


/**
 * Get information about the threads
 */
void threads_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char msg[BUFSIZE] = {0, };
        char *ptr;
        int n;

        (void) options;
        (void) nb_options;

        ptr = strtok(options, " \n");
        if (!ptr){
                _send_threads_info(fd);
                return;
        }

        if (strcmp(ptr, "inc")==0 && cfg->nb_threads<MAX_THREADS)
                n = snprintf(msg, BUFSIZE, "Nb threads level is now %d\n", ++cfg->nb_threads);
        else if (strcmp(ptr, "dec")==0 && cfg->nb_threads>1)
                n = snprintf(msg, BUFSIZE, "Nb threads level is now %d\n", --cfg->nb_threads);
        else
                n = snprintf(msg, BUFSIZE, "Invalid action\n Syntax\n threads (inc|dec)\n");

        proxenet_write(fd, (void*)msg, n);

        return;
}


/**
 * Enumerate all plugins available in the plugins directory
 */
static int plugin_cmd_list_available(sock_t fd)
{
        struct dirent *dir_ptr=NULL;
	DIR *dir = NULL;
	char* name = NULL;
        char msg[2048] = {0, };
        int n, type;
        bool first_iter = true;

        dir = opendir(cfg->plugins_path);
	if (dir == NULL) {
		xlog(LOG_ERROR, "Failed to open '%s': %s\n", cfg->plugins_path, strerror(errno));
                n = snprintf(msg, sizeof(msg), "{\"Error\": \"Cannot open '%s': %s\"}",
                             cfg->plugins_path, strerror(errno));
                proxenet_write(fd, msg, n);
		return -1;
	}

        n = snprintf(msg, sizeof(msg),"{\"Plugins available in %s\": {",
                     cfg->plugins_path);
        proxenet_write(fd, msg, n);
	while ((dir_ptr=readdir(dir))) {
                type = -1;
                name = dir_ptr->d_name;
		if (!strcmp(name,".") || !strcmp(name,".."))
                        continue;

                type = proxenet_get_plugin_type(name);
		if (type < 0)
                        continue;

                /* this is only to avoid breaking json syntax */
                if (!first_iter)
                        proxenet_write(fd, ",", 1);
                else
                        first_iter = false;


                proxenet_xzero(msg, sizeof(msg));
                n = snprintf(msg, sizeof(msg), "\"%s\": [\"%s\", %d]",
                             name, supported_plugins_str[type],
                             proxenet_is_plugin_loaded(name)?1:0);

                proxenet_write(fd, msg, n);
        }
        proxenet_write(fd, "}}", 2);

        if (closedir(dir) < 0){
		xlog(LOG_ERROR, "Failed to close '%s': %s\n", cfg->plugins_path, strerror(errno));
                n = snprintf(msg, sizeof(msg), "{\"Error\": \"Cannot close dir '%s': %s\"",
                             cfg->plugins_path, strerror(errno));
                proxenet_write(fd, msg, n);
		return -1;
        }

        return 0;
}


/**
 * Set plugin properties
 */
static int plugin_cmd_set(sock_t fd, char *options)
{
        int ret, n;
        char msg[BUFSIZE] = {0, };
        char *ptr;
        unsigned int plugin_id;

        plugin_id = (unsigned int)atoi(options);
        if (plugin_id > proxenet_plugin_list_size()) {
                n = sprintf(msg, "Invalid plugin id\n");
                proxenet_write(fd, (void*)msg, n);
                return -1;
        }

        ptr = strtok(NULL, " \n");
        if(!ptr){
                n = snprintf(msg, BUFSIZE, "Invalid set command syntax: plugin set %d <command>\n", plugin_id);
                proxenet_write(fd, (void*)msg, n);
                return -1;
        }

        if (!strcmp(ptr, "toggle")){
                ret = proxenet_toggle_plugin(plugin_id);
                if (ret < 0){
                        n = snprintf(msg, BUFSIZE, "Failed to toggle plugin %d\n", plugin_id);
                        proxenet_write(fd, (void*)msg, n);
                        return -1;
                }

                n = snprintf(msg, BUFSIZE, "Plugin %d is now %sACTIVE\n", plugin_id, ret?"":"IN");
                proxenet_write(fd, (void*)msg, n);
                return 0;
        }

        if (!strcmp(ptr, "priority")){
                ptr = strtok(NULL, " \n");
                if(!ptr){
                        n = snprintf(msg, BUFSIZE,
                                     "Missing priority argument: plugin set %d prority <prio>\n",
                                     plugin_id);
                        proxenet_write(fd, (void*)msg, n);
                        return -1;
                }

                n = atoi(ptr);
                if (n==0){
                        proxenet_write(fd, (void*)"Invalid priority\n", 17);
                        return -1;
                }

                if (proxenet_plugin_set_prority(plugin_id, n) < 0){
                        n = snprintf(msg, BUFSIZE,
                                     "An error occured during priority update for plugin %d\n",
                                     plugin_id);
                        proxenet_write(fd, (void*)msg, n);
                        return -1;
                }

                n = snprintf(msg, BUFSIZE, "Plugin %d priority is now %d\n", plugin_id, n);
                proxenet_write(fd, (void*)msg, n);
                return 0;
        }

        n = snprintf(msg, BUFSIZE, "Unknown action '%s' for plugin %d\n", ptr, plugin_id);
        proxenet_write(fd, (void*)msg, n);

        return -1;
}


/**
 * Load a plugin during runtime
 */
static int plugin_cmd_load(sock_t fd, char *options)
{
        char* plugin_name = NULL;
        int ret, n;
        char msg[BUFSIZE] = {0, };
        char *ptr;
        size_t l;

        ptr = strtok(options, " \n");
        if (!ptr){
                proxenet_write(fd, (void*)"Missing plugin name\n", 20);
                return -1;
        }

        l = strlen(ptr);
        plugin_name = alloca(l+1);
        proxenet_xzero(plugin_name, l+1);
        memcpy(plugin_name, ptr, l);

        if ( plugin_name==NULL ){
                n = sprintf(msg, "proxenet_xstrdup2() failed: %s\n", strerror(errno));
                proxenet_write(fd, (void*)msg, n);
                return -1;
        }

        ret = proxenet_add_new_plugins(cfg->plugins_path, plugin_name);
        if(ret<0){
                n = snprintf(msg, sizeof(msg)-1, "Error while loading plugin '%s'\n", plugin_name);
                proxenet_write(fd, (void*)msg, n);
                return -1;
        }

        if(ret)
                n = snprintf(msg, sizeof(msg)-1, "Plugin '%s' added successfully\n", plugin_name);
        else
                n = snprintf(msg, sizeof(msg)-1, "File '%s' has not been added\n", plugin_name);
        proxenet_write(fd, (void*)msg, n);

        /* plugin list must be reinitialized since we dynamically load VMs only if plugins are found */
        proxenet_initialize_plugins();

        return 0;
}


/**
 *
 */
static int plugin_cmd_change_all_status(proxenet_state new_state)
{
        plugin_t *p;
        proxenet_state old_state;
        int retcode;

        switch (new_state){
                case INACTIVE:
                        old_state = ACTIVE;
                        break;

                case ACTIVE:
                        old_state = INACTIVE;
                        break;

                default:
                        xlog(LOG_ERROR, "Invalid state %d\n", new_state);
                        return -1;
        }

        for (p=plugins_list; p!=NULL; p=p->next) {
                if (p->state != old_state)
                        continue;

                retcode = proxenet_plugin_set_state(p->id, new_state);
                if (retcode < 0){
                        xlog(LOG_ERROR, "Failed to set state %ud to plugin %ud\n", new_state, p->id);
                }
        }

        return 0;
}


/**
 * Handle plugins (list/activate/deactivate/load)
 */
void plugin_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char msg[BUFSIZE] = {0, };
        char *ptr;
        int n, res;

        (void) options;
        (void) nb_options;


        /* usage */
        ptr = strtok(options, " \n");
        if (!ptr)
                goto invalid_plugin_action;

        if (strcmp(ptr, "list") == 0) {
                plugin_cmd_list_json(fd, true);
                return;
        }
        if (strcmp(ptr, "list-all") == 0) {
                plugin_cmd_list_available(fd);
                return;
        }
        if (strcmp(ptr, "enable-all") == 0) {
                plugin_cmd_change_all_status(ACTIVE);
                return;
        }
        if (strcmp(ptr, "disable-all") == 0) {
                plugin_cmd_change_all_status(INACTIVE);
                return;
        }
        if (strcmp(ptr, "set") == 0) {
                /* shift argument */
                ptr = strtok(NULL, " \n");
                if (!ptr){
                        xlog(LOG_ERROR, "%s\n", "Failed to get 'set' argument");
                        return;
                }
                res = plugin_cmd_set(fd, ptr);
                if(res < 0){
                        xlog(LOG_ERROR, "%s\n", "An error occured during plugin setting");
                }
                return;
        }
        if (strcmp(ptr, "load") == 0) {
                /* shift argument */
                ptr = strtok(NULL, " \n");
                if (!ptr){
                        xlog(LOG_ERROR, "%s\n", "Failed to get 'load' argument");
                        return;
                }
                res = plugin_cmd_load(fd, ptr);
                if(res < 0){
                        xlog(LOG_ERROR, "%s\n", "An error occured during plugin loading");
                }
                return;
        }


invalid_plugin_action:
        n = snprintf(msg, BUFSIZE,
                     "Invalid action.\nSyntax\n"
                     "plugin [list][list-all][enable-all][disable-all][set <id> toggle][load <0PluginName.ext>]\n");
        proxenet_write(fd, (void*)msg, n);
        return;
}


/**
 * Edit configuration
 */
void config_cmd(sock_t fd, char *options, unsigned int nb_options)
{
        char msg[BUFSIZE] = {0, };
        char *ptr;
        int n;

        (void) options;
        (void) nb_options;

        /* usage */
        ptr = strtok(options, " \n");
        if (!ptr)
                goto invalid_config_action;

        if (strcmp(ptr, "ssl-intercept") == 0) {
                /* shift argument */
                ptr = strtok(NULL, " \n");
                if (!ptr){
                        xlog(LOG_ERROR, "%s\n", "Failed to get argument");
                        return;
                }

                if ( strcasecmp(ptr, "true") == 0){
                        cfg->ssl_intercept = true;
                        if (cfg->verbose)
                                xlog(LOG_INFO, "%s\n", "[config] Enabled SSL intercept");
                        proxenet_write(fd, "SSL intercept enabled\n", 22);
                        return;
                } else if ( strcasecmp(ptr, "false") == 0){
                        cfg->ssl_intercept = false;
                        if (cfg->verbose)
                                xlog(LOG_INFO, "%s\n", "[config] Disabled SSL intercept");
                        proxenet_write(fd, "SSL intercept disabled\n", 23);
                        return;
                }
        }

invalid_config_action:
        n = snprintf(msg, BUFSIZE,
                     "Invalid action.\nSyntax\n"
                     "config [ssl-intercept true|false]\n");
        proxenet_write(fd, (void*)msg, n);
        return;
}


/**
 * Parse an incoming command
 *
 * @param name is a pointer to the buffer of the command to parse
 * @return pointer to valid struct command_t if `name` is found as known_commands[]
 * @return NULL otherwise
 */
struct command_t* get_command(char *name)
{
        struct command_t *cmd;
        char *buf = name;
        char c;

        do {
                c = *buf;
                if (c == '\0')
                        return NULL;
                if (c=='\n' || c==' ') {
                        *buf = '\0';
                        break;
                }

                buf++;
        } while (1);

        for (cmd=known_commands; cmd && cmd->name; cmd++) {
                if (strcmp(name, cmd->name) == 0) {
                        *buf = c;
                        return cmd;
                }
        }

        return NULL;
}


/**
 * Main handler for new control command
 */
int proxenet_handle_control_event(sock_t* sock) {
        char read_buf[BUFSIZE] = {0, };
        char *ptr = NULL;
        int retcode = -1;
        struct command_t *cmd = NULL;

        retcode = proxenet_read(*sock, read_buf, BUFSIZE-1);
        if (retcode < 0) {
                xlog(LOG_ERROR, "Failed to read control command: %s\n", strerror(errno));
                return -1;
        }

        if (retcode == 0) {
                return -1;
        }

        if (read_buf[0] == '\n') {
                goto cmd_end;
        }

        if ( (cmd=get_command(read_buf)) == NULL ) {
                proxenet_write(*sock, CONTROL_INVALID, strlen(CONTROL_INVALID));
                goto cmd_end;
        }

        if(cfg->verbose)
                xlog(LOG_INFO, "Receiving control command: \"%s\" \n", cmd->name);

        ptr = &read_buf[strlen(cmd->name)];
        cmd->func(*sock, ptr, cmd->nb_opt_max);

cmd_end:
        proxenet_write(*sock, CONTROL_PROMPT, strlen(CONTROL_PROMPT));

        return 0;
}
