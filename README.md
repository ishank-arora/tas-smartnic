# BlueTAS

Built on top of [TAS](https://github.com/tcp-acceleration-service/tas).

BlueTAS is a way of offloading the fast and slow path of TAS from the CPU of a machine to the cores of a SmartNIC. 

## How to run

Some defintions. A host refers to the main compute machine on which applications are run. Inside the host, there is a SmartNIC. We can run applications on SmartNIC too. So, on the host, we run:

`arp -s [IP_address_of_SmartNIC] [MAC_address_of_SmartNIC]`

`tas_host/tas_host`

and for each application, we run

`LD_PRELOAD=lib/libtas_interpose.so application_binary application_args`

On the SmartNIC, we run:

`sudo tas_nic/tas_nic --ip-addr=10.0.0.170/24 --fp-cores-max=2 --nic-ip=10.0.0.170 --nic-port=39244 --shm-len=536870912 --nic-port=39244 `

where `10.0.0.170` is the ip-address of the smartNIC. 
