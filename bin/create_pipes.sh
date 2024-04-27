#!/bin/sh

rm -f /tmp/computational_module.*

mkfifo /tmp/computational_module.out
mkfifo /tmp/computational_module.in
