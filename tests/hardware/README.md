# Hardware soak tests

These are the throughput/soak scripts the driver was validated with on real
hardware (STM32N6, i.MX RT1062 and Alif Ensemble E3 stations against a HaLow
AP on channel 28 at 2 MHz bandwidth).  They need a MicroPython embedding of
the driver (the `network.HALOW` binding) and a HaLow access point; they are
run by hand, not by CI.

- `thru_server.py` -- run on a host reachable through the AP: a TCP (9001) and
  UDP (9002) throughput peer.
- `thru_device.py` -- run on the station: associates, then cycles TCP/UDP
  up/down transfers against the server for 20 minutes, printing per-cycle
  rates.  Edit `HOST` to the server's address and provide `halow_config.py`
  (see `halow_config.py.example`) with the AP credentials.
- `thru_file.py` -- as above, but drop-tolerant: results are appended to a
  file on the device so a run survives the USB console dropping.  Useful for
  long RF soaks.

A healthy 2 MHz station sees on the order of 8 Mbit/s UDP up on a Cortex-M55
class host with the IRQ line wired, and association in single-digit seconds;
sustained losses or watchdog-style stalls indicate an integration problem
(polling cadence, IRQ wiring, or SPI signal integrity) rather than RF.
