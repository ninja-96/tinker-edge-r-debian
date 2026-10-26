# Basic security of the image (release 0.3)

Two measures, both on by default. They cost no measurable speed.

## Firewall (nftables)

`/etc/nftables.conf`, loaded by `nftables.service` at boot. Incoming traffic is dropped except:

| Allowed | Why |
|---|---|
| loopback, answers to connections the board opened, ICMP echo and error messages, IPv6 neighbour and router discovery | normal operation of the network |
| DHCP answers (UDP 68 from 67, DHCPv6 546 from 547) | the address is obtained before a connection is tracked |
| TCP 22 | SSH (unchanged: password login, no rate limit) |
| UDP 5353 | mDNS, so that `tinker-edge-r.local` works |

Outgoing traffic is not restricted. To open another port add a line such as `tcp dport 8080 accept`
to the `input` chain and run `sudo systemctl restart nftables`. To switch the firewall off:
`sudo systemctl disable --now nftables`.

## Password change at the first login

The user `debian` is created with the password `debian` and an expired password (`chage -d 0`), so
the first login, over SSH or on the console, asks for a new one before anything else. Build an image
without it (for automated tests) with `FORCE_PW_CHANGE=0 ./run.sh scripts/build-rootfs.sh`.
`scripts/test-board.sh`: log in once by hand, change the password, then run it with `BOARD_PASS=<new>`.

## Not done on purpose

Secure boot through eFuses (cannot be undone), a secure OS (TrustZone only hosts BL31 for PSCI), SELinux
(dropped). Cheap extra measures that are not scheduled are listed in [ROADMAP.md](ROADMAP.md).
