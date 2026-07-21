# Robot firewall (rockrobo)

Script: `/mnt/data/rockctl/bin/firewall.sh`  
Boot: applied from `/mnt/reserve/_root.sh`

## SSH is always allowed

```
INPUT -p tcp --dport 22 -j ACCEPT
OUTPUT -p tcp --sport 22 -j ACCEPT
```

Do not ship a ruleset without those rules.

## Commands

```bash
/mnt/data/rockctl/bin/firewall.sh apply
/mnt/data/rockctl/bin/firewall.sh status
/mnt/data/rockctl/bin/firewall.sh stop   # flush; still leaves SSH ACCEPT
```

## What is blocked (questionable)

| Direction | Target | Why |
|-----------|--------|-----|
| IN | UDP/TCP 54321–54323 from non-lo | miio discovery / control from guests |
| IN | TCP 80/443/1883/8883/5037 | old HTTP, adbd, MQTT |
| IN | rockctl :8080 from non-RFC1918 | API not on public net |
| OUT | TCP 80/443/1883/8883/5222/5223 | cloud / phone-home |
| OUT | UDP 123 | public NTP |
| OUT | non-LAN TCP (catch-all) | exfil / unknown |
| OUT | UDP 54321 to non-LAN | miio cloud |

## What is allowed

- **SSH :22** any source  
- rockctl **:8080** from 10/8, 172.16/12, 192.168/16  
- nanobot **:8787** from private nets  
- lo, private outbound, DNS, DHCP  
- ICMP from private  

## Kernel limits

No `conntrack`/`state`/`comment`/`multiport` modules — rules use basic tcp/udp/icmp matches only.
