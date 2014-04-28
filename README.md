## Instructions 

    make deps compile

## Running

    ./bin/dabloom_http
    Usage: ./bin/dablooms_http <bloom_dir> <global_bloom_words_file>

    mkdir /tmp/blooms_dir
    touch /tmp/bootstrap_additions.txt
    
    ./bin/dabloom_http /tmp/blooms_dir/ /usr/share/dict/words

## Usage

    curl http://localhost:9003/?key=orange
    #NOTE: PLAIN TEXT
    0

    curl -X POST -d 'key=orange' http://localhost:9003/
    #NOTE: JSON
    {"ok":0}

    curl http://localhost:9003/?key=orange
    1

    curl -X POST 'ns=cities&key=pune' http://localhost:9003/
    {"ok":1}
 
    curl http://localhost:9003/?key=pune
    0
 
    curl http://localhost:9003/?key=pune&ns=cities
    1

    curl http://location:9003/?metrics=1
    {"queries":6, "hits":6, "misses":0, "additions":1, "namespaces":2}

