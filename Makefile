.PHONY: clean all compile deps docs
DIR=`pwd`
deps:
	git clone https://github.com/bitly/dablooms.git
	git clone https://github.com/petewarden/c_hashmap
	git clone https://github.com/cesanta/mongoose.git

all:
	make deps compile run

run:
	${DIR}/bin/dablooms_http	

clean:
	rm -rf ${DIR}/build

compile: prepare-build compile-dablooms compile-hashmap compile-mongoose
	@cc -L${DIR}/build/mongoose.so -L${DIR}/build/dablooms.so -L${DIR}/build/mursur.so -L${DIR}/build/hashmap.so ${DIR}/dablooms/src/dablooms.c ${DIR}/mongoose/mongoose.c ${DIR}/c_hashmap/hashmap.c ${DIR}/dablooms/src/murmur.c main.c -o ${DIR}/bin/dablooms_http -pthread -W -Wall -std=c99 -pedantic -O2 -I ${DIR}/dablooms/src/ -I${DIR}/mongoose -I${DIR}/c_hashmap  -I/usr/local/include/ -lm
	echo "Done compiling"

prepare-build:
	#rm -rf ${DIR}/build
	mkdir -p ${DIR}/build ${DIR}/bin

compile-dablooms:
	@cd ${DIR}/dablooms && make install  BLDDIR=../build DESTDIR=../build 
  	@cc ${DIR}/dablooms/src/dablooms.c -shared -fPIC -fpic -o ${DIR}/build/dablooms.so -ldl -pthread -W -Wall -std=c99 -pedantic -O2 -I ${DIR}/dablooms/src/
	@cc ${DIR}/dablooms/src/murmur.c -shred -fPIC -fpic -o ${DIR}/build/murmur.so -ldl -pthread -W _Wall -std=c99 -pedantic -O2 -I ${DIR}/dablooms/src/

compile-hashmap:
	@cc ${DIR}/c_hashmap/hashmap.c -shared -fPIC -fpic -o ${DIR}/build/hashmap.so -ldl -pthread -W -Wall -std=c99 -pedantic -O2 -I ${DIR}/c_hashmap/

compile-mongoose:
	@cc ${DIR}/mongoose/mongoose.c -shared -fPIC -fpic -o ${DIR}/build/mongoose.so -ldl -pthread -W -Wall -std=c99 -pedantic -O2 -I ${DIR}/mongoose/

docs:
	echo "todo"
