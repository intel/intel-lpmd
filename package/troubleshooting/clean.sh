#!/bin/bash
#purpose: script to clean tmp files

sudo make clean

cd ../../

rm -r .deps 
rm -r Makefile    
rm -r Makefile.in
rm -r aclocal.m4  
rm -r autom4te.cache  
rm -r build-aux  
rm -r config.h  
rm -r config.h.in 
rm -r config.log 
rm -r config.status 
rm -r configure 
rm -r configure~  
rm -r data/Makefile  
rm -r data/Makefile.in  
rm -r gtk-doc.make  
rm -r m4
rm -r stamp-h1
rm *.o

rm -r src/.deps  
rm src/.dirstamp
rm src/*.o

rm -r src/wlt_proxy/.deps  
rm src/wlt_proxy/.dirstamp  
rm src/wlt_proxy/*.o

rm -r src/additional/.deps  
rm src/additional/.dirstamp
rm src/additional/*.o

rm -r src/visualize/.deps  
rm src/visualize/.dirstamp
rm src/visualize/*.o

rm -r src/weights/.deps/
rm src/weights/.dirstamp
rm src/weights/*.o

rm package/troubleshooting/clean

#delete intermediate files and output binaries
rm data/intel_lpmd.service
rm data/org.freedesktop.intel_lpmd.service
rm intel_lpmd
rm intel_lpmd_dbus_interface.h
rm lpmd-resource.c
rm tools/intel_lpmd_control


