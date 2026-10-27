#!/usr/bin/env python3
"""Provisiona WiFi por Improv Serial sobre el puerto CDC del S2 Mini.

  python improv_provision.py --check
  python improv_provision.py "MiRed" "miclave"
"""
import argparse, sys, time, serial

STATES = {1: 'AwaitingAuthorization', 2: 'Authorized', 3: 'Provisioning', 4: 'Provisioned'}
ERRORS = {0: 'None', 1: 'InvalidRpc', 2: 'UnknownRpc', 3: 'UnableToConnect',
          4: 'NotAuthorized', 0xFF: 'Unknown'}
TYPES = {1: 'CurrentState', 2: 'ErrorState', 3: 'Rpc', 4: 'RpcResponse'}


def build(ftype, payload):
    p = b'IMPROV' + bytes([1, ftype, len(payload)]) + payload
    return p + bytes([sum(p) & 0xFF])


def rpc(cmd, data=b''):
    return build(0x03, bytes([cmd, len(data)]) + data)


def parse(buf):
    """Extrae tramas completas; devuelve (tramas, resto)."""
    out = []
    while True:
        i = buf.find(b'IMPROV')
        if i < 0 or len(buf) < i + 10:
            break
        dlen = buf[i + 8]
        end = i + 9 + dlen + 1
        if len(buf) < end:
            break
        f = buf[i:end]
        if (sum(f[:-1]) & 0xFF) == f[-1]:
            out.append((f[7], f[9:9 + dlen]))
        buf = buf[end:]
    return out, buf


def show(ftype, payload):
    name = TYPES.get(ftype, ftype)
    if ftype == 1:
        print(f"  <- {name}: {STATES.get(payload[0], payload[0])}")
    elif ftype == 2:
        print(f"  <- {name}: {ERRORS.get(payload[0], payload[0])}")
    elif ftype == 4:
        strs, p = [], payload[1:]
        while p:
            n = p[0]; strs.append(p[1:1 + n].decode('utf-8', 'replace')); p = p[1 + n:]
        print(f"  <- {name}: {strs}")
    else:
        print(f"  <- {name}: {payload.hex(' ')}")


def pump(s, buf, seconds):
    """Lee durante unos segundos; devuelve (resto, tramas vistas)."""
    seen = []
    end = time.time() + seconds
    while time.time() < end:
        c = s.read(512)
        if c:
            buf += c
            frames, buf = parse(buf)
            for t, p in frames:
                show(t, p)
                seen.append((t, p))
    return buf, seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ssid', nargs='?')
    ap.add_argument('password', nargs='?', default='')
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--check', action='store_true', help='solo consultar el estado')
    a = ap.parse_args()
    if not a.check and not a.ssid:
        ap.error('hace falta un SSID, o --check')

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.2
    # DTR bajo es lo que decide: el CDC del ROM reinicia cuando RTS cae, y
    # entra en modo descarga si DTR esta alto en ese instante.
    s.dtr, s.rts = False, True
    s.open()
    time.sleep(0.3)
    print("-> reiniciando a la app (RTS 1->0 con DTR bajo)")
    s.rts = False
    time.sleep(2.0)

    # Abrir el puerto puede reiniciar la placa (el CDC del ROM reinicia con
    # las lineas de control), asi que se espera y se sondea hasta que conteste.
    buf = b''
    for intento in range(1, 6):
        time.sleep(1.5)
        s.reset_input_buffer()
        print(f"-> GetCurrentState (intento {intento})")
        s.write(rpc(0x02)); s.flush()
        buf, seen = pump(s, buf, 2.0)
        if seen:
            break
    else:
        print("La placa no contesta. Pulsa RST y reintenta.")
        s.close(); return 1

    if a.check:
        s.close(); return 0

    data = bytes([len(a.ssid)]) + a.ssid.encode() + bytes([len(a.password)]) + a.password.encode()
    print(f"-> WifiSettings ssid={a.ssid!r} ({len(data)} bytes de payload)")
    s.write(rpc(0x01, data)); s.flush()
    buf, _ = pump(s, buf, 25.0)
    s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
