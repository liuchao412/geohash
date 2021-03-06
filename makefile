include makefile.define

default:all

PATS = ./GeoHash/geohash.o \
	   ./GeoHash/mapinfo.o \
	   ./GeoHash/posinfopool.o \
	   ./GeoHash/areainfopool.o \
	   ./GeoHash/ShareMemory.o \
	   ./Http/soapC.o \
	   ./Http/soapServer.o \
	   ./Http/httppost.o \
	   ./Http/stdsoap2.o \
	   ./Http/json.o \
	   main.o

LIB_BASE_OBJS = geohash.o \
	            mapinfo.o \
	            posinfopool.o \
	            areainfopool.o \
	            ShareMemory.o \
	            soapC.o \
	            soapServer.o \
	            httppost.o \
	            stdsoap2.o \
	            json.o \
	            main.o

LIB_BASE = mapchinapos

all: mapchinapos

# ?????
all:$(LIB_BASE) makefile

$(LIB_BASE):$(PATS)
	$(CC) -rdynamic -o $(LIB_BASE) $(LIB_BASE_OBJS) $(LIBS)

# ??
clean:
	rm -rf *.o  $(LIB_BASE) $(LIB_BASE_OBJS)
cl:
	rm -rf *.o 
