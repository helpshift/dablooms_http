//
// dablooms_http
// wraps mongoose web-server and a hash-map over the dablooms scalable bloomfilters, with support for namespacing
//
//  main.c
//
// Usage:
// dablooms_http    --folder=<blooms_dir> # -f; no trailing slash
// Optional :  --port=<port>      # -p; default 9003
// Optional :  --bootstrap=<file> # -b;
// Optional :  --test             # -t; turn test mode on
// Optional :  --daemon           # -d; turn daemon mode on
// every membership addition is of the form
// => POST key=foo OR
// => POST key=foo&ns=mynamespacing
//
// every membership query is of the form
// => GET ?key=foo OR
// => GET ?key=foo&ns=mynamespacing
//
// in which case a seperate scalable bloom filter is maintained for each namespace
// when no ns is specified, global.bf is used
//
//  Created by Bhasker Kode<bosky@helpshift.com> on 27/04/14.
//  Copyright (c) 2014 HelpShift Inc. All rights reserved.
//


#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "mongoose.h"
#include "hashmap.h"
#include "dablooms.h"

#include "constants.h"

// storing command line options
struct dablooms_http_options {
    int is_test;
    int is_daemon;
    char *port;
    char *bloom_dir;
    char *bootstrap;
} dablooms_http_options = {TEST,DAEMON_ON,PORT_LISTEN,NULL,NULL};

// usage metrics
struct dablooms_http_metrics {
    int query_hits;
    int query_misses;
    int additions;
    int queries;
    int namespaces;
} dablooms_http_metrics = {0,0,0,0,0};

// global helper
struct Server {
    scaling_bloom_t *bloom;
    int i;
    map_t kv;
    struct dablooms_http_metrics metrics;
    struct dablooms_http_options start_options;
} Server;

// all hashmap get's or set are of this form
typedef struct data_struct_s
{
    char key_string[KEY_MAX_LENGTH];
    scaling_bloom_t *bloom;
} data_struct_t;


struct Server server = {.i = 0};

void get_command_line_opts(int argc, char *argv[])
{
    int c;
    int long_index = 0;
    static struct option longopt[] = {
        {"folder", required_argument, 0,  'f' },
        {"port", optional_argument, 0,  'p' },
        {"test", optional_argument, 0,  't' },
        {"daemon", optional_argument, 0,  'd' },
        {"bootstrap", optional_argument, 0,  'b' },
        {0,0,0,0}
    };
    while ((c = getopt_long(argc, argv, "f:p:d:t:b:", longopt, &long_index)) != -1)
        switch (c)
    {
        case 'f':
            /**
             * looks in this folder for saved bloom filters
             * every regular file inside the bloom_dir folder will be loaded back into memory
             */
            server.start_options.bloom_dir = realpath (optarg, NULL);
            printf("\n--folder %s",server.start_options.bloom_dir);
            break;
        case 'p':
            /**
             * which port to start the web server on
             */
            server.start_options.port = optarg;
            printf("\n--port %s",server.start_options.port);
            break;
        case 'd':
            /**
             * turn on/off daemon mode
             */
            server.start_options.is_daemon = 1;
            printf("\n--daemon %d",server.start_options.is_daemon);
            break;
        case 't':
            /**
             * if is test, adds CAPACITY entries for every namespace
             */
            server.start_options.is_test = 1;
            printf("\n--test %d",server.start_options.is_test);
            break;
        case 'b':
            /**
             * bootstrap all entries in this file into the global namespace
             */
            server.start_options.bootstrap = realpath (optarg, NULL);
            printf("\n--bootstrap %s",server.start_options.bootstrap);
            break;
        default:
            break;
    }
    if(!server.start_options.port){
        server.start_options.port = PORT_LISTEN;
    }
}

scaling_bloom_t* reopen_scaling_bloom(const char *bloom_file)
{
    FILE *fp;
    scaling_bloom_t *bloom;
    
    if (!(fp = fopen(bloom_file, "r"))) {
        fclose(fp);
        remove(bloom_file);
        return NULL;
    }
    
    if (!(bloom = new_scaling_bloom_from_file(CAPACITY, ERROR_RATE, bloom_file))) {
        fprintf(stderr, "ERROR: Could not create bloom filter\n");
        return NULL;
    }
    
    fseek(fp, 0, SEEK_SET);
    fclose(fp);
    
    return bloom;

}

static void chomp_line(char *word)
{
    char *p;
    if ((p = strchr(word, '\r'))) {
        *p = '\0';
    }
    if ((p = strchr(word, '\n'))) {
        *p = '\0';
    }
}

scaling_bloom_t* get_scaling_bloom(unsigned int capacity, double error_rate, const char *bloom_file, const char *words_file)
{

    scaling_bloom_t *bloom = new_scaling_bloom(capacity, error_rate, bloom_file);

    FILE *fp;
    if(words_file){
        if (!(fp = fopen(words_file, "r"))) {
            fprintf(stderr, "ERROR: Could not open words file %s\n",words_file);
            return bloom;
        }

        char word[256];
        int i,ctr=0;

        for (i = 0; fgets(word, sizeof(word), fp); i++,ctr++) {
            chomp_line(word);
            scaling_bloom_add(bloom, word, strlen(word), i);
        
        }
        
        if(server.start_options.is_test){
            printf("\n* test with 1 million words\n");
            for (i = 0; i < CAPACITY; i++, server.i++) {
                ctr++;
                sprintf(word, "word%d", i); // puts string into buffer
                scaling_bloom_add(bloom, word, strlen(word),i);
            }
        }
        printf("\nAdded %d words \n",ctr);
        
        //fseek(fp, 0, SEEK_SET); // to overwrite file next
        fclose(fp);
    }
    return bloom;
}

scaling_bloom_t* get_bloom_for_request(char *ns)
{
    int error;
    if(!strlen(ns)){
        return server.bloom;
    }else{
        data_struct_t *value;
        char filename[500];
        sprintf(filename,"%s/%s.bf",server.start_options.bloom_dir,ns);
        error = hashmap_get(server.kv, filename, (void **)(&value));
        if (error==MAP_OK){
            return value->bloom;
        }else{
            value = malloc(sizeof(data_struct_t));
            value->bloom = new_scaling_bloom(CAPACITY, ERROR_RATE, filename);
            snprintf(value->key_string, KEY_MAX_LENGTH, "%s/%s", KEY_PREFIX, filename);
            hashmap_put(server.kv, ns, value);
            server.metrics.namespaces++;
            return value->bloom;
        }
    }
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev) {
    int result = MG_FALSE;
    char reply[100];
    if (ev == MG_REQUEST) {
        char key[500];
        mg_get_var(conn, "key", key, sizeof(key));
        char ns[500];
        //TODO use uri as ns
        // sprintf(ns, "%s", conn->uri);
        mg_get_var(conn, "ns", ns, sizeof(ns));

        scaling_bloom_t *bloom = get_bloom_for_request(ns);
        if (!strcmp(conn->request_method, "POST")){
            mg_send_header(conn, "Content-Type", "application/json");
            if(!scaling_bloom_check(bloom, key, strlen(key))){
                sprintf(reply, "{\"ok\":%d}",server.i);
                server.metrics.additions++;
                scaling_bloom_add(bloom, key, strlen(key),server.i++);
            }else{
                sprintf(reply, "{\"error\":\"exists\"}");
            }
        }else{
            char key[500];
            mg_get_var(conn, "key", key, sizeof(key));
            if(strlen(key)){
                server.metrics.queries++;
                if(scaling_bloom_check(bloom, key, strlen(key))){
                    server.metrics.query_hits++;
                    sprintf(reply, "1");
                }else{
                    server.metrics.query_misses++;
                    sprintf(reply, "0");
                }
            }else{
                mg_get_var(conn, "metrics", key, sizeof(key));
                if(strlen(key)){
                    mg_send_header(conn, "Content-Type", "application/json");
                    sprintf(reply, "{\"queries\":%d, \"hits\":%d, \"misses\":%d, \"additions\":%d, \"namespaces\":%d}",server.metrics.queries, server.metrics.query_hits, server.metrics.query_misses, server.metrics.additions, server.metrics.namespaces);
                }else{
                    return result;
                }
            }
        }
        result = MG_TRUE;
        mg_printf_data(conn, reply);
    }else if (ev == MG_AUTH) {
        result = MG_TRUE;
    }
    return result;
}

void show_warning()
{
    fprintf(stderr, "Usage:\ndablooms_http    --folder=<blooms_dir> # -f; no trailing slash\n");
    fprintf(stderr, "     Optional :  --port=<port>      # -p; default 9003\n");
    fprintf(stderr, "     Optional :  --bootstrap=<file> # -b; \n");
    fprintf(stderr, "     Optional :  --test             # -t; to turn test mode on\n");
    fprintf(stderr, "     Optional :  --daemon           # -d; to turn daemon mode on\n");
}

int start_loop(int argc, char *argv[])
{
    /**
     * To run the server, you pass in two arguments
     * where to store the data for the bloom filters
     * and location of a text file to load by default into the global namespace
     * => ./dablooms /data/blooms/ /tmp/default_words_in_global.txt
     */
    if (!server.start_options.bloom_dir) {
        show_warning();
        return EXIT_FAILURE;
    }

    /**
     * we have a hashmap that simply maintains something of the form
     * => kv[dir/<ns>.bf] = bloom;
     */
    server.kv = hashmap_new();

    DIR           *d;
    
    char * dir_name = server.start_options.bloom_dir;
    
    /* Open the bloom_dir directory. */
    d = opendir (dir_name);
    int errno=0;
    if (d) {
        while (1) {
            struct dirent * entry;
            entry = readdir (d);
            if (! entry) {
                break;
            }
            if (!strcmp (entry->d_name, "."))
                continue;
            if (!strcmp (entry->d_name, ".."))
                continue;
            if (!strcmp (entry->d_name, "global.bf"))
                continue;
            char filename[500];

            /* each name here is already named <ns.bf>, convert this to dir/<ns.bf> */
            sprintf(filename,"%s/%s",server.start_options.bloom_dir,entry->d_name);
            
            /* reference to the bloom filter, which is the value */
            scaling_bloom_t *bloom = reopen_scaling_bloom(filename);
            if(!bloom){
                bloom = new_scaling_bloom_from_file(CAPACITY, ERROR_RATE, filename);
            }
            /* reference to the key */
            data_struct_t *value = malloc(sizeof(data_struct_t));
            value->bloom = bloom;
            snprintf(value->key_string, KEY_MAX_LENGTH, "%s/%s", KEY_PREFIX, filename);
  
            /* set it into the hashmap */
            hashmap_put(server.kv, filename, value);
            server.metrics.namespaces++;
        }
            
        /* Close the directory. */
        if (closedir (d)) {
            fprintf (stderr, "Could not close '%s': %s\n",dir_name, strerror (errno));
            exit (EXIT_FAILURE);
        }
    }
    
    char global_bloom[100];
    sprintf(global_bloom, "%s/global.bf",server.start_options.bloom_dir);
    if(server.start_options.bootstrap){
        /* load the global bloom filter */
        char bootstrap_global_bf_file[500];
        sprintf(bootstrap_global_bf_file, "%s", server.start_options.bootstrap);
        server.bloom = get_scaling_bloom(CAPACITY, ERROR_RATE, global_bloom, bootstrap_global_bf_file);
    }else{
        server.bloom = get_scaling_bloom(CAPACITY, ERROR_RATE, global_bloom, NULL);
    }
    server.metrics.namespaces++;
    
    if (!server.bloom) {
        show_warning();
        return EXIT_FAILURE;
    }else{
        /**
         * web server
         */

        struct mg_server *http_server;
        
        /* Create and configure the server */
        http_server = mg_create_server(NULL, ev_handler);
        mg_set_option(http_server, "listening_port", server.start_options.port);
        
        printf("\nStarting on port %s\n", mg_get_option(http_server, "listening_port"));
        for (;;) {
            mg_poll_server(http_server, 1000);
        }
        
        /* Cleanup, and free server instance */
        mg_destroy_server(&http_server);
        
        return 0;
    }
}

int main(int argc, char *argv[])
{
    get_command_line_opts(argc, argv);
    int pid;
    if(server.start_options.is_daemon){
        
        pid=fork();  /* Duplicate. Child and parent continue from here.*/
        if (pid!=0)  /* pid is non-zero, so I must be the parent  */
        {
            printf("\ndablooms_http started as daemon with PID %d.\n", pid);
            return 0;
        }
        else  /* pid is zero, so I must be the child. */
        {
            
            return start_loop(argc,argv);
        }
    }else{
        return start_loop(argc,argv);
    }
    
}