#!/bin/sh

F=/tmp/check.sh.$$
trap 'rm -f $F' 0

mkdir include/linux -p
cp /usr/local/google/home/joelaf/repo/linux-master/include/linux/rcu_segcblist.h include/linux
cp /usr/local/google/home/joelaf/repo/linux-master/include/linux/rcupdate.h include/linux
cp /usr/local/google/home/joelaf/repo/linux-master/include/linux/atomic.h include/linux
cp /usr/local/google/home/joelaf/repo/linux-master/kernel/rcu/rcu_segcblist.h rcu_segcblist.h
cp /usr/local/google/home/joelaf/repo/linux-master/kernel/rcu/rcu_segcblist.c rcu_segcblist.c
cc -g -o test_rcu_segcblist -Iempty_includes -Iinclude test_rcu_segcblist.c
./test_rcu_segcblist > $F
cmp $F rcu_segcblist.out
