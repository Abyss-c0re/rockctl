#!/bin/sh
# rockrobo lab firewall — ALWAYS keep SSH (tcp/22) open.
# No conntrack on this kernel. Grok session HTTPS (443) allowed for nanobot peer.

IPT=/sbin/iptables
[ -x "$IPT" ] || IPT=$(command -v iptables)

log() { echo "firewall: $*" >&2; }

apply() {
  log "applying (SSH :22 always accepted first)"
  $IPT -P INPUT ACCEPT
  $IPT -P FORWARD ACCEPT
  $IPT -P OUTPUT ACCEPT
  $IPT -F INPUT
  $IPT -F OUTPUT
  $IPT -F FORWARD

  # ========== INPUT ==========
  $IPT -A INPUT -i lo -j ACCEPT

  # SSH — NEVER remove / never put after DROP
  $IPT -A INPUT -p tcp --dport 22 -j ACCEPT

  # Without conntrack, outbound clients need return traffic allowed by sport.
  # HTTPS replies (Grok) and DNS replies — do this BEFORE high-port DROPs.
  $IPT -A INPUT -p tcp --sport 443 -j ACCEPT
  $IPT -A INPUT -p udp --sport 53 -j ACCEPT
  $IPT -A INPUT -p tcp --sport 53 -j ACCEPT

  # rockctl API — private nets
  $IPT -A INPUT -p tcp --dport 8080 -s 192.168.0.0/16 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8080 -s 10.0.0.0/8 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8080 -s 172.16.0.0/12 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8080 -j DROP

  # nanobot UI + peer bus for other Grok sessions — private nets
  $IPT -A INPUT -p tcp --dport 8787 -s 192.168.0.0/16 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8787 -s 10.0.0.0/8 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8787 -s 172.16.0.0/12 -j ACCEPT
  $IPT -A INPUT -p tcp --dport 8787 -j DROP

  # miio — only loopback (local rockctl/firmware); not LAN guests
  $IPT -A INPUT -p udp --dport 54321 -i lo -j ACCEPT
  $IPT -A INPUT -p udp --dport 54321 -j DROP
  $IPT -A INPUT -p tcp --dport 54321 -j DROP
  $IPT -A INPUT -p tcp --dport 54322 -j DROP
  $IPT -A INPUT -p tcp --dport 54323 -j DROP

  $IPT -A INPUT -p tcp --dport 5037 -j DROP
  $IPT -A INPUT -p tcp --dport 80 -j DROP
  $IPT -A INPUT -p tcp --dport 443 -j DROP
  $IPT -A INPUT -p tcp --dport 8883 -j DROP
  $IPT -A INPUT -p tcp --dport 1883 -j DROP
  $IPT -A INPUT -p tcp --dport 8888 -j DROP
  $IPT -A INPUT -p tcp --dport 29 -j DROP

  $IPT -A INPUT -p icmp -s 192.168.0.0/16 -j ACCEPT
  $IPT -A INPUT -p icmp -s 10.0.0.0/8 -j ACCEPT
  $IPT -A INPUT -p icmp -s 172.16.0.0/12 -j ACCEPT
  $IPT -A INPUT -p icmp -j DROP

  # close common unsolicited TCP ranges (SSH/8080/8787 already accepted above)
  $IPT -A INPUT -p tcp --dport 1:21 -j DROP
  $IPT -A INPUT -p tcp --dport 23:79 -j DROP
  $IPT -A INPUT -p tcp --dport 81:442 -j DROP
  $IPT -A INPUT -p tcp --dport 444:5036 -j DROP
  $IPT -A INPUT -p tcp --dport 5038:8079 -j DROP
  $IPT -A INPUT -p tcp --dport 8081:8786 -j DROP
  $IPT -A INPUT -p tcp --dport 8788:65535 -j DROP
  # UDP: drop known-bad low services only. Do NOT drop high UDP ports —
  # without conntrack that kills DNS replies (ephemeral dports).
  $IPT -A INPUT -p udp --dport 1:52 -j DROP
  $IPT -A INPUT -p udp --dport 54:66 -j DROP
  $IPT -A INPUT -p udp --dport 69:122 -j DROP
  $IPT -A INPUT -p udp --dport 124:1900 -j DROP
  $IPT -A INPUT -p udp --dport 1901:5352 -j DROP
  $IPT -A INPUT -p udp --dport 5354:54320 -j DROP
  # 54321 already handled above; leave 54322+ open for DNS client replies etc.

  # ========== OUTPUT ==========
  $IPT -A OUTPUT -o lo -j ACCEPT
  $IPT -A OUTPUT -p tcp --sport 22 -j ACCEPT
  $IPT -A OUTPUT -d 192.168.0.0/16 -j ACCEPT
  $IPT -A OUTPUT -d 10.0.0.0/8 -j ACCEPT
  $IPT -A OUTPUT -d 172.16.0.0/12 -j ACCEPT
  $IPT -A OUTPUT -d 127.0.0.0/8 -j ACCEPT

  $IPT -A OUTPUT -p udp --dport 53 -j ACCEPT
  $IPT -A OUTPUT -p tcp --dport 53 -j ACCEPT
  $IPT -A OUTPUT -p udp --dport 67:68 -j ACCEPT

  # Grok browser-session + inference (nanobot peer / activation)
  $IPT -A OUTPUT -p tcp --dport 443 -j ACCEPT

  # block questionable OEM phone-home / mqtt / xmpp (not Grok)
  $IPT -A OUTPUT -p tcp --dport 80 -j DROP
  $IPT -A OUTPUT -p tcp --dport 8883 -j DROP
  $IPT -A OUTPUT -p tcp --dport 1883 -j DROP
  $IPT -A OUTPUT -p tcp --dport 5222 -j DROP
  $IPT -A OUTPUT -p tcp --dport 5223 -j DROP
  $IPT -A OUTPUT -p udp --dport 123 -j DROP

  $IPT -A OUTPUT -p udp --dport 54321 -d 192.168.0.0/16 -j ACCEPT
  $IPT -A OUTPUT -p udp --dport 54321 -d 10.0.0.0/8 -j ACCEPT
  $IPT -A OUTPUT -p udp --dport 54321 -d 127.0.0.0/8 -j ACCEPT
  $IPT -A OUTPUT -p udp --dport 54321 -j DROP

  # remaining non-LAN TCP outbound
  $IPT -A OUTPUT -p tcp -j DROP

  $IPT -P FORWARD DROP
  $IPT -P INPUT ACCEPT
  $IPT -P OUTPUT ACCEPT

  # belt: SSH rule must exist
  if ! $IPT -S INPUT | grep -q -- '--dport 22'; then
    $IPT -I INPUT 1 -p tcp --dport 22 -j ACCEPT
  fi
  log "applied; SSH tcp/22 ACCEPT present"
  $IPT -S INPUT | head -8
}

status() { $IPT -L -n -v; echo ---; $IPT -S; }

case "${1:-apply}" in
  apply|start) apply ;;
  status) status ;;
  stop|flush)
    $IPT -P INPUT ACCEPT
    $IPT -P OUTPUT ACCEPT
    $IPT -P FORWARD ACCEPT
    $IPT -F INPUT
    $IPT -F OUTPUT
    $IPT -F FORWARD
    $IPT -I INPUT 1 -p tcp --dport 22 -j ACCEPT
    log "flushed; SSH still explicitly allowed"
    ;;
  *) echo "Usage: $0 apply|status|stop"; exit 2 ;;
esac
