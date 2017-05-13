CC = gcc

DEPS =  scr-statuscode.h	\
	scr-network.h

# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CFLAGS += -Wall -g
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(shell sdl2-config --cflags) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS)) $(shell sdl2-config --libs) $(LDLIBS)
	
SCROOBY = 	scrooby-sender	\
		scrooby-player

OBJS = 		scrooby-sender.o	\
		scrooby-player.o	\
		scr-network.o

all: scrooby-sender scrooby-player

scr-network.o: scr-network.c scr-network.h 
	$(CC) $(CFLAGS) -c scr-network.c

scrooby-sender: scrooby-sender.o
	gcc scrooby-sender.o -o scrooby-sender $(CFLAGS) $(LDLIBS)

scrooby-player: scrooby-player.o scr-network.o
	gcc scrooby-player.o scr-network.o -o scrooby-player $(CFLAGS) $(LDLIBS)

clean-test:
	$(RM) test.*

clean: clean-test
	$(RM) $(SCROOBY) $(OBJS)
