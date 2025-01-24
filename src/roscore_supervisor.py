#!/usr/bin/env python

import rosnode 
import time
import subprocess

dt = 10
max_attempts = 6

def get_rosout_pid():
    try:
        info = subprocess.check_output(['rosnode', 'info', '/rosout']).decode('utf-8')
        for line in info.split('\n'):
            if 'Pid' in line:
                return int(line.split()[-1])
    except subprocess.CalledProcessError:
        return None

pid = None

while True:
    pid = get_rosout_pid()
    if pid is not None:
        break
    print("Waiting for roscore to start...")
    time.sleep(dt)

print("rosout PID:", pid)

current_attempts = 0

while True:
    current_pid = get_rosout_pid()
    if current_pid is None:
        if current_attempts < max_attempts:
            print("rosout node not found. Waiting...")
            current_attempts += 1
        else:
            print("rosout node is assumed dead. Exiting.")
            exit(1)
    else:
        current_attempts = 0

    if current_pid != pid:
        print("rosout node has been restarted. Exiting.")
        exit(1)

    time.sleep(dt)