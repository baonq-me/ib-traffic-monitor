# ib-traffic-monitor

## Intro

`ib-traffic-monitor` is a tool to monitor the local InfiniBand device metrics under Linux operating system. It is written in C and utilizing [ncurses](https://invisible-island.net/ncurses/) as the TUI library.

It can also run headless with `-d`, exposing the same metrics through a built-in [Prometheus](https://prometheus.io/) exporter web server instead of drawing the TUI.

## Screenshot

![](screenshots/ib-traffic-monitor.png)

## Metrics List

`ib-traffic-monitor` reports following metrics

| Category | Metric Name | Unit | Description |
| --- | --- | --- | --- |
| Interface Status | LID | n/a | Local Identifier |
| Interface Status | Link Layer | n/a | link layer type |
| Interface Status | State | n/a | port state |
| Interface Status | Physical State | n/a | port physical state |
| Interface Status | Rate | n/a | port data rate |
| Interface I/O | RX Packet | packet/second | number of received packets per second |
| Interface I/O | RX MB | MB/second | number of received bytes per second |
| Interface I/O | TX Packet | packet/second | number of transmitted packets per second |
| Interface I/O | TX MB | MB/second | number of transmitted bytes per second |
| Interface I/O | UC RX Packet | packet/second | number of received unicast packets per second |
| Interface I/O | UC TX Packet | packet/second | number of transmitted unicast packets per second |
| Interface I/O | MC RX Packet | packet/second | number of received multicast packets |
| Interface I/O | MC TX Packet | packet/second | number of transmitted multicast packets |
| Interface Error | Symbol | count | number of minor link errors detected on one or more physical lanes |
| Interface Error | RX | packet | number of packets containing an error that were received on the port |
| Interface Error | RX Remote PHY | packet | number of packets marked with the EBP delimiter received on the port |
| Interface Error | RX Switch Relay | packet | number of packets received on the port that were discarded |
| Interface Error | RX Const. | packet | number of packets received on the switch physical port that are discarded |
| Interface Error | TX Const. | packet | number of packets not transmitted from the switch physical port |
| Interface Error | Buffer Overrun | count | number of input buffer overrun |
| Interface Error | TX Discard | packet | number of outbound packets discarded |
| Interface Error | VL15 Dropped | packet | number of incoming VL15 packets dropped |
| Interface Link Error | Link Error Recovery | count | number of times the Port Training state machine has successfully completed the link error recovery process |
| Interface Link Error | Link Error Downed | count | number of times the Port Training state machine has failed the link error recovery process and downed the link |
| Interface Link Error | Local Link Integrity | count | number of times the count of local physical errors exceeded the threshold |

## Compilation

```
$ make
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -c -o ib-traffic-monitor.o ib-traffic-monitor.c
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -c -o infiniband.o infiniband.c
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -c -o utils.o utils.c
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -c -o ncurses_utils.o ncurses_utils.c
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -c -o prometheus.o prometheus.c
gcc -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined -I. -o ib-traffic-monitor ib-traffic-monitor.o infiniband.o utils.o ncurses_utils.o prometheus.o -lncurses
```

## Usage

Users can simply run `ib-traffic-monitor` without any options. The default refresh period is 5 seconds.

If `ib-traffic-monitor` cannot not detect any valid InfiniBand device, the program will exit with error.

```
$ ./ib-traffic-monitor -h
InfiniBand Traffic Monitor - Version 1.5.0
usage: ib-traffic-monitor [-r|--refresh <second(s)>]
                          [-e|--ethernet]
                          [-m|--memory-lock]
                          [-d|--daemon]
                          [-l|--listen <ip address>]
                          [-p|--port <port>]
                          [-h|--help]
```

`-r` or `--refresh`: specify the refresh period. the unit is second. it has no effect in daemon mode

`-e` or `--ethernet`: show Ethernet link layer type devices. the default behavior is showing InfiniBand link layer devices only

`-m` or `--memory-lock`: enable memory lock on application

`-d` or `--daemon`: run headless, without the TUI, and serve metrics over HTTP for Prometheus

`-l` or `--listen`: IPv4 address the Prometheus exporter binds to. the default address is 0.0.0.0, which is every interface. it has no effect without `-d`

`-p` or `--port`: listening port of the Prometheus exporter. the default port is 19315. it has no effect without `-d`

`-h` or `--help`: show help message

## Headless Mode

With `-d`, no TUI is drawn. The program binds the listen address on the exporter port, `0.0.0.0:19315` by default, and serves:

| Path | Description |
| --- | --- |
| `/metrics` | metrics in the Prometheus text exposition format |
| `/` | a landing page linking to `/metrics` |

```
$ ./ib-traffic-monitor -d -l 127.0.0.1 -p 19315
InfiniBand Traffic Monitor - Version 1.5.0
Prometheus exporter listening on http://127.0.0.1:19315/metrics
press Ctrl-C to exit
```

Every metric carries a `hostname` label holding the host name reported by `gethostname()` at scrape time. Each scrape reads sysfs directly, so no data is buffered between scrapes and the exporter is stateless. Counters are exported raw and cumulative, exactly as the kernel reports them - `rate()` in PromQL replaces the per-second columns of the TUI. Data counters are converted from the 4-byte words used by sysfs into bytes.

| Metric | Type | Labels | Description |
| --- | --- | --- | --- |
| `ib_exporter_build_info` | gauge | `hostname`, `version` | build information, always 1 |
| `ib_ports_total` | gauge | `hostname` | number of ports discovered in the last scrape |
| `ib_port_info` | gauge | `hostname`, `device`, `port`, `link_layer`, `state`, `phys_state`, `rate` | static port information, always 1 |
| `ib_port_lid` | gauge | `hostname`, `device`, `port` | Local Identifier |
| `ib_port_state` | gauge | `hostname`, `device`, `port` | numeric port state, -1 when unknown |
| `ib_port_physical_state` | gauge | `hostname`, `device`, `port` | numeric physical port state, -1 when unknown |
| `ib_port_receive_packets_total` | counter | `hostname`, `device`, `port` | received packets |
| `ib_port_transmit_packets_total` | counter | `hostname`, `device`, `port` | transmitted packets |
| `ib_port_receive_data_bytes_total` | counter | `hostname`, `device`, `port` | received bytes |
| `ib_port_transmit_data_bytes_total` | counter | `hostname`, `device`, `port` | transmitted bytes |
| `ib_port_unicast_receive_packets_total` | counter | `hostname`, `device`, `port` | received unicast packets |
| `ib_port_unicast_transmit_packets_total` | counter | `hostname`, `device`, `port` | transmitted unicast packets |
| `ib_port_multicast_receive_packets_total` | counter | `hostname`, `device`, `port` | received multicast packets |
| `ib_port_multicast_transmit_packets_total` | counter | `hostname`, `device`, `port` | transmitted multicast packets |
| `ib_port_symbol_errors_total` | counter | `hostname`, `device`, `port` | minor link errors on one or more physical lanes |
| `ib_port_receive_errors_total` | counter | `hostname`, `device`, `port` | received packets containing an error |
| `ib_port_receive_remote_physical_errors_total` | counter | `hostname`, `device`, `port` | received packets marked with the EBP delimiter |
| `ib_port_receive_switch_relay_errors_total` | counter | `hostname`, `device`, `port` | received packets that were discarded |
| `ib_port_receive_constraint_errors_total` | counter | `hostname`, `device`, `port` | received packets discarded on the switch physical port |
| `ib_port_transmit_constraint_errors_total` | counter | `hostname`, `device`, `port` | packets not transmitted from the switch physical port |
| `ib_port_excessive_buffer_overrun_errors_total` | counter | `hostname`, `device`, `port` | input buffer overruns |
| `ib_port_transmit_discards_total` | counter | `hostname`, `device`, `port` | discarded outbound packets |
| `ib_port_vl15_dropped_total` | counter | `hostname`, `device`, `port` | dropped incoming VL15 packets |
| `ib_port_link_error_recovery_total` | counter | `hostname`, `device`, `port` | successful link error recovery processes |
| `ib_port_link_downed_total` | counter | `hostname`, `device`, `port` | failed link error recovery processes that downed the link |
| `ib_port_local_link_integrity_errors_total` | counter | `hostname`, `device`, `port` | times the count of local physical errors exceeded the threshold |

Example Prometheus scrape configuration:

```yaml
scrape_configs:
  - job_name: ib-traffic-monitor
    static_configs:
      - targets: ['localhost:19315']
```

Example queries:

```
rate(ib_port_receive_data_bytes_total[5m])          # RX bytes/second per port
rate(ib_port_receive_errors_total[5m]) > 0          # ports taking receive errors
ib_port_state != 4                                  # ports that are not ACTIVE
```

## ChangeLog

```
[03/24/2025] 1.0.0 - initial commit

[03/25/2025] 1.1.0 - add flag to show Ethernet link layer type device

[05/11/2025] 1.2.0 - optimize program quitting in non-blocking style

[05/16/2025] 1.3.0 - handle SIGINT signal gracefully

[05/20/2025] 1.3.1 - fix typo on port_rcv_constraint_errors metric

[05/24/2025] 1.3.2 - update version string

[09/02/2025] 1.3.3 - fix variable shadowing

[05/12/2026] 1.4.0 - add flag to enable memory lock

[05/16/2026] 1.4.1 - fix stack overflow issue when device count is greater than INTERFACE_COUNT

[09/13/2026] 1.5.0 - add headless mode serving a Prometheus exporter web server
```

## Use Case

* Demos from [GNA-G Community](https://www.gna-g.net/) in SC25:

[Demo](https://sc25.supercomputing.org/wp-content/uploads/2025/11/nre128s3.pdf)

[Presentation](https://www.gna-g.net/wp-content/uploads/2025/12/rednesp_sc25_widescreen-v1.6.pdf)

```
The demonstration successfully featured the simultaneous transfer of data from multiple Brazilian universities to the exhibition hall at SC25.

The ib-traffic-monitor tool was essential, enabling both the initial equipment setup and the continuous monitoring of the production links used throughout the demo.
```

## Reference

[Linux sysfs interface common for all infiniband devices](https://www.kernel.org/doc/Documentation/ABI/stable/sysfs-class-infiniband)

[Understanding mlx5 Linux Counters and Status Parameters](https://enterprise-support.nvidia.com/s/article/understanding-mlx5-linux-counters-and-status-parameters)
