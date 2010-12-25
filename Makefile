# kbbind - Utility for remapping Linux keyboard inputs
# Copyright (C) 2010 Daniel Collins <solemnwarning@solemnwarning.net>
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#
#     * Neither the name of the author nor the names of its contributors may
#       be used to endorse or promote products derived from this software
#       without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

VERSION := 1.1

CXX := g++
CXXFLAGS := -Wall -DVERSION=$(VERSION)
INCLUDES :=
LIBS :=
INSTALL := install
GZIP := gzip

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

all: kbbind

clean:
	rm -f kbbind *.o

install: all
	$(INSTALL) -D -m 0755 kbbind $(BINDIR)
	
	$(GZIP) -c kbbind.1 > kbbind.1.gz
	$(INSTALL) -D -m 0644 kbbind.1.gz $(MANDIR)/man1/

uninstall:
	rm -f $(BINDIR)/bin/kbbind
	rm -f $(MANDIR)/man1/kbbind.1.gz

kbbind: kbbind.o
	$(CXX) $(CXXFLAGS) -o kbbind kbbind.o $(LIBS)

kbbind.o: kbbind.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o kbbind.o kbbind.cpp
