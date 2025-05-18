from os import listdir
import random
import concurrent.futures
import pexpect
import os
import subprocess
from time import sleep
import multiprocessing
import threading
import sys 

def is_running(pid):
    """
    Returns True if a process with given pid is running, False otherwise.
    """
    try:
        os.kill(pid, 0)  # Does not terminate the process
    except OSError:
        return False
    return True

def run_simulation(cmd, sudo_password, lock, active_pids_dict):
    os.sched_setaffinity(0, set(range(18)))
    full_cmd = f"./ns3 run {cmd}"
    pid=''
    try:
        child = ''
        sleep(5)
        with lock:
            print(f"Running: {full_cmd}")
            child = pexpect.spawn(full_cmd, encoding='utf-8', timeout=300000000)
            sleep(2.5)

        pid = child.pid
        print(pid)
        active_pids_dict[pid] = True
        child.expect(pexpect.EOF)
        output = child.before
    except Exception as e:
        output = f"[ERROR] Exception occurred: {e}"
        
    
    print(output)
    return output

def monitor_pids(sudo_password, lock,active_pids_dict):
    # sleep()
    c=0
    while True:
        cmd = "ps -eo pid,psr,ni,comm --sort=-psr | grep 'ns3.38-Linear' | awk '{print $1}'"
    
        output = subprocess.check_output(cmd, shell=True, text=True)
        
        pids = output.strip().split()


        # c=(c+1)%100
        # if c==1:

        #         print("configuring")
        #         cfg = pexpect.spawn(
        #             "./ns3 configure --build-profile=optimized --out=build/optimized",
        #             encoding='utf-8', timeout=300000000
        #         )
        #         cfg.wait()         # blocks until the configure command exits
        #         if cfg.exitstatus != 0:
        #             raise RuntimeError(f"configure failed: {cfg.exitstatus}")

        #         print("building")
        #         build = pexpect.spawn("./ns3 build", encoding='utf-8', timeout=300000000)
        #         build.expect(pexpect.EOF)
        #         build.close()
        #         print("done")
        for pid in pids:
            # print(f"Renicing PID {pid} to priority -20...")
            command = ["sudo", "renice", "-n", "-10", "-p", pid]
            subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

        
        
        sleep(10)

def main():
    sudo_password = "hoolaeho"  
    manager = multiprocessing.Manager()
    lock = manager.Lock()
    active_pids_dict = manager.dict()
    loc = ""
    a=listdir(f'./pacn/csma/{loc}')
    nodes=[2,4,8,16,32,64]
    rates=[10, 20, 50, 100, 200, 500]
    commands = []

    for i, n in enumerate(nodes):
        for j, rate in enumerate(rates):
                    s=f'{i}_{j}.txt'
                    if s not in a:
                        cmd = (
                        f"\"scratch/csma.cc "
                        f"--n={n} "
                        f"--lambda={rate} "
                        f"--out={i}_{j}\""
                        )
                        commands.append(cmd)

    print(f"Total number of simulations to run: {len(commands)}\n")
    if len(commands)==0:
        return -1;
    monitor_thread = threading.Thread(
        target=monitor_pids,
        args=(sudo_password,lock, active_pids_dict),
        daemon=True  
    )
    monitor_thread.start()
    max_workers = 18
    results = []
    debug_error = False
    # commands=commands[:min(len(commands),25)]
    with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as executor:
        if not debug_error:
            futures = [executor.submit(run_simulation, cmd, sudo_password,lock,active_pids_dict) for cmd in commands]
        for future in concurrent.futures.as_completed(futures):
            result_output = future.result()
            if "[ERROR]" in result_output:
                results.append(result_output)
            if "[ERROR] Debug mode detected" in result_output:
                debug_error=True

    print("\n=== All Simulations Complete ===\n")
    for idx, output in enumerate(results, 1):
        print(f"--- Simulation #{idx} output ---")
        print(output)
        print("--------------------------------\n")

    if debug_error:
        return -2 

if __name__ == "__main__":
   
    out = main()
        
