CC=gcc

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

SCROOBY=        scrooby-sender                     \
	       	scrooby-player

OBJS=$(addsuffix .o,$(SCROOBY))

all: $(OBJS) $(SCROOBY)

clean-test:
	$(RM) test.*

clean: clean-test
	$(RM) $(SCROOBY) $(OBJS)
