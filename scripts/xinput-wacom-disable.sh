#!/bin/sh
for id in $(xinput | grep Wacom | perl -ne '/id=(\d+)/ && print "$1\n";') ; do
	xinput disable $id
done
