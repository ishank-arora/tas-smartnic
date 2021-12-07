# BlueTAS

Built on top of [TAS](https://github.com/tcp-acceleration-service/tas).

BlueTAS is an implementation of offloading the fast and slow path of TAS from the CPU of the host machine to the cores of a SmartNIC. 

Here is our [report](Report.pdf) the details BlueTAS' functionality. 

## Overview

The repo consists of three main components - `lib`, `tas_host` and `tas_nic`. The interaction between the three and the application can be summarized in the following figure:

```
------------------        --------------    |    -------------
|  App  | libTAS |  <==>  |  tas_host  |  <=|=>  |  tas_nic  |
------------------        --------------    |    -------------
                    HOST                    |         NIC
```

`lib` consists of the application interface which is needed to be preloaded as a shared object when running any application.  
`tas_host` is the part of the TAS stack that is implemented to be run on the host - specifically, it houses the implementatio to connect and flush to the RDMA queues on the NIC.  
`tas_nic` is the fast and slow path of TAS that runs on the NIC.  

## How to build

On the host, the only folders that need to be made are the `lib/` and `tas_host` folders. For convenience, we provide a modified Makefile which accounts for that. So, on the host, run:

```bash
$ make -f MakefileHost
```

On the SmartNic, we simply just build all the folders. Assuming that dpdk is installed in `~/dpdk-inst` TAS can be built as follows:

```bash
$ make RTE_SDK=~/dpdk-inst
```

## How to run

*Some definitions:* A host refers to the main compute machine on which applications are run. Inside the host, there is a SmartNIC. We can run applications on SmartNIC too.

### BlueTAS Setup
So, on the host, we run:

```bash
$ arp -s [IP_address_of_SmartNIC] [MAC_address_of_SmartNIC]
$ tas_host/tas_host
```

On the SmartNIC, we run:

```bash
$ sudo tas_nic/tas_nic --ip-addr=10.0.0.170/24 --fp-cores-max=2 --nic-ip=10.0.0.170 --nic-port=39244 --shm-len=536870912
```

where `10.0.0.170` is the ip-address of the smartNIC. 

### Run

To run an application using TAS, we run

```bash
$ LD_PRELOAD=lib/libtas_interpose.so application_binary application_args
```
