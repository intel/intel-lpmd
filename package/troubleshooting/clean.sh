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
rm -r src/.deps  
rm -r src/.dirstamp  
rm -r src/wlt_proxy/.deps  
rm -r src/wlt_proxy/.dirstamp  
rm -r stamp-h1
