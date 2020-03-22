# 2RFA
fork process misconfigurated
patch osmocom-bb
patch openbsc
copy the .h Files in ~/osmocom-bb/src/host/layer23/src/mobile/ in osmocom-bb
copy the .h File in openbsc/src/libmsc/ in openbsc
invert the port between server.h and client.h 666-888
put your plmn imsi in ~/.osmocom/bb/mobile.cfg
set auth to a5 1 in openbsc.cfg
in terminal : 
    telnet 0 4242 
    en
    suberiber imsi 012345678901234 xor a3a8 0123456789abcdef0123456789abcdef
prepare mobile
run openbsc 
when phone connect whait to the server 
quick! run mobile 
and with wireshark can see the result
