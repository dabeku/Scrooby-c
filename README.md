# Scrooby-c

A high performance video and audio stream processor for Scrooby 

## Specification

We use a MPEG-TS container with
* H264 for video
* MP2 for audio

## Setup

The following procedure was done with a Mac but can easily be done with any OS.

### Requirements:

```
brew install yasm
```

### libx264 (video)
```
1. git clone http://git.videolan.org/git/x264.git x264
2. ./configure --prefix=/usr/local --enable-shared
3. make
4. sudo make install
```

### libopus (audio, currently not used)

```
1. brew install automake
2. git clone git://git.opus-codec.org/opus.git
3. cd opus
4. ./autogen.sh
5. ./configure --enable-static --enable-shared
6. make
7. sudo make install
```

### ffmpeg (codec)
```
1. git clone https://git.ffmpeg.org/ffmpeg.git ffmpeg
2. ./configure  --prefix=/usr/local --enable-gpl --enable-nonfree --enable-libfreetype --enable-libx264 --enable-libopus
3. make
4. sudo make install
```

### SDL2.0 (threads, UI, mutex)

See ![Installation Guide](https://wiki.libsdl.org/Installation)

```
1. Download latest version: https://www.libsdl.org/download-2.0.php
2. Unpack into new folder
3. Change to new folder
```

Better way (this will assure to be compatible with other OSx version)
```
4. mkdir build ; cd build ; CC=/where/i/cloned/SDL/build-scripts/gcc-fat.sh ../configure ; make
```

Also works (may cause incompatibilities)
```
4. ./configure
5. make
6. sudo make install (defaults: /usr/local/lib + /usr/local/include/SDL2 + sdl2-config)
```

## Building

```
make clean
make
```

## Testing

You can easily test this application by streaming data from your device to a UDP endpoint.

UDP (low bandwidth, much CPU)
```
ffmpeg -s 640x480 -r 30 -f avfoundation -i "0:0" -c:v libx264 -tune zerolatency -pix_fmt yuv420p -f mpegts udp://127.0.0.1:1234
```

UDP (high bandwidth, low CPU)
```
ffmpeg -s 640x480 -r 30 -f avfoundation -i "0:0" -c:v libx264 -tune zerolatency -preset ultrafast -pix_fmt yuv420p -f mpegts udp://127.0.0.1:1234
```

TCP
```
ffmpeg -s 640x480 -r 30 -f avfoundation -i "0:0" -c:v libx264 -tune zerolatency -pix_fmt yuv420p -c:a aac -b:a 128k -f mpegts pipe:1 | nc -l -k 51234
```

## Commands

List all devices (video and audio)
```
ffmpeg -f avfoundation -list_devices true -i ""
