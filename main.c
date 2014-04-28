//
// light multi-bloomfilter server
// wraps mongoose web-server and a hash-map over the dablooms scalable bloom filter code
//
//  main.c
//
//  Created by Bhasker Kode<bosky@helpshift.com> on 27/04/14.
//  Copyright (c) 2014 HelpShift Inc. All rights reserved.
//

/**
 * every membership addition is of the form
 * => POST key=foo OR
 * => POST key=foo&ns=mongo.mynamespacing

 * every membership query is of the form
 * => GET ?key=foo OR
 * => GET ?key=foo&ns=mongo.mynamespacing
 *
 * in which case a seperate scalable bloom filter is maintained for each namespace
 * when no ns is specified, global.bf is used
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dirent.h>

#include "mongoose.h"
#include "hashmap.h"
#include "dablooms.h"

#include "constants.h"

struct dablooms_http_metrics {
    int query_hits;
    int query_misses;
    int additions;
    int queries;
    int namespaces;
} dablooms_http_metrics = {0,0,0,0,0};

struct Server {
    scaling_bloom_t *bloom;
    int i;
    map_t kv;
    char *bloom_dir;
    struct dablooms_http_metrics metrics;
} Server;

typedef struct data_struct_s
{
    char key_string[KEY_MAX_LENGTH];
    scaling_bloom_t *bloom;
} data_struct_t;


struct Server server = {.i = 0};

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
    
    if (!(fp = fopen(words_file, "r"))) {
        fprintf(stderr, "ERROR: Could not open words file\n");
        return NULL;
    }

    char word[256];
    int i,ctr=0;

    for (i = 0; fgets(word, sizeof(word), fp); i++,ctr++) {
        if (i % 2 == 0) {
            chomp_line(word);
            scaling_bloom_add(bloom, word, strlen(word), i);
        }
    }

    if(TEST){
        printf("\n* test with 1 million words\n");
        for (i = 0; i < CAPACITY; i++, server.i++) {
            ctr++;
            sprintf(word, "word%d", i); // puts string into buffer
            scaling_bloom_add(bloom, word, strlen(word),i);
        }
    }
    printf("Added i=%d ctr=%d words \n",i,ctr);
    
    //fseek(fp, 0, SEEK_SET);
    fclose(fp);
    
    return bloom;
}

scaling_bloom_t* get_bloom_for_request(char *ns)
{
    int error;
    if(!strlen(ns)){
        return server.bloom;
    }else{
        data_struct_t *value;
        char filename[100];
        sprintf(filename,"%s%s.bf",server.bloom_dir,ns);
        error = hashmap_get(server.kv, filename, (void **)(&value));
        if (error==MAP_OK){
            return value->bloom;
        }else{
            value = malloc(sizeof(data_struct_t));
            value->bloom = new_scaling_bloom(CAPACITY, ERROR_RATE, filename);
            snprintf(value->key_string, KEY_MAX_LENGTH, "%s%s", KEY_PREFIX, filename);
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

int start_loop(int argc, char *argv[])
{
    /**
     * To run the server, you pass in two arguments
     * where to store the data for the bloom filters
     * and location of a text file to load by default into the global namespace
     * => ./dablooms /data/blooms/ /tmp/default_words_in_global.txt
     */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <bloom_dir> <global_bloom_words_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /**
     * we have a hashmap that simply maintains something of the form
     * => kv[dir/<ns>.bf] = bloom;
     */
    server.kv = hashmap_new();

    /**
     * every regular file inside the bloom_dir folder ( first argument ) will be loaded back into memory
     */
    server.bloom_dir = argv[1];
    
    DIR           *d;
    
    char * dir_name = server.bloom_dir;
    
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
            char filename[100];

            /* each name here is already named <ns.bf>, convert this to dir/<ns.bf> */
            sprintf(filename,"%s%s",server.bloom_dir,entry->d_name);
            
            /* reference to the bloom filter, which is the value */
            scaling_bloom_t *bloom = reopen_scaling_bloom(filename);
            if(!bloom){
                bloom = new_scaling_bloom_from_file(CAPACITY, ERROR_RATE, filename);
            }
            /* reference to the key */
            data_struct_t *value = malloc(sizeof(data_struct_t));
            value->bloom = bloom;
            snprintf(value->key_string, KEY_MAX_LENGTH, "%s%s", KEY_PREFIX, filename);
  
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
    
    /* load the global bloom filter */
    char global_bloom[100];
    sprintf(global_bloom, "%sglobal.bf",server.bloom_dir);
    char bootstrap_global_bf_file[100];
    sprintf(bootstrap_global_bf_file, "%s", argv[2]);
    server.bloom = get_scaling_bloom(CAPACITY, ERROR_RATE, global_bloom, bootstrap_global_bf_file);
    server.metrics.namespaces++;
    
    if (!server.bloom) {
        return EXIT_FAILURE;
    }else{
        /**
         * web server
         */

        struct mg_server *server;
        
        /* Create and configure the server */
        server = mg_create_server(NULL, ev_handler);
        mg_set_option(server, "listening_port", PORT_LISTEN);
        
        printf("Starting on port %s\n", mg_get_option(server, "listening_port"));
        for (;;) {
            mg_poll_server(server, 1000);
        }
        
        /* Cleanup, and free server instance */
        mg_destroy_server(&server);
        
        return 0;
    }
}

int main(int argc, char *argv[])
{
    
    int pid;
    if(DAEMON_ON){
        
        pid=fork();  /* Duplicate. Child and parent continue from here.*/
        if (pid!=0)  /* pid is non-zero, so I must be the parent  */
        {
            printf("dablooms started as daemon with PID %d.\n", pid);
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