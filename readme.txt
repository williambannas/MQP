 compile for pluto!

 CC=arm-xilinx-linux-gnueabi-gcc CFLAGS=--sysroot=../staging LDFLAGS=--sysroot=../staging make

 scp dump1090 root@192.168.2.1:/sbin/

 ./tcp_client 192.168.0.104 1337 POST /api/pluto '{"pid": "1", "RSS": "-40 dB"}' "Content-Type: application/json"
 