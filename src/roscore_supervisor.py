#!/usr/bin/env python

import rosnode 
import time
import subprocess

timeout = 10

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
    time.sleep(timeout)

print("rosout PID:", pid)

while True:
    current_pid = get_rosout_pid()
    if current_pid is None:
        print("rosout node is not alive. Exiting.")
        exit(1)
    if current_pid != pid:
        print("rosout node has been restarted. Exiting.")
        exit(1)

    time.sleep(timeout)