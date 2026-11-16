# Concepts: how the pieces fit

```
  device                                        your computer                    AWS IoT Core
  ──────                                        ─────────────                    ────────────
  makes a private key (never leaves)
  CSR (public key + name)  ───── scp / USB ───▶  aws-iot-register  ──────────▶  thing  rpi-ex-<id>
                                                                                 certificate (signed from the CSR)
  certificate + endpoint  ◀───── scp / USB ────  device.crt, config.json  ◀──── policy rpi-ex-client
  connects: TLS with key + certificate  ═══════════════════════════════════▶  endpoint (MQTT, port 8883)
```

**Thing.** A device's entry in the AWS IoT registry, named here `rpi-ex-<id>`: a Raspberry Pi's machine ID from its firmware (its 64-bit serial on older firmware), or a Pico's board ID. The device uses it as its MQTT client ID.

**Certificate.** An X.509 certificate that AWS IoT signs from the device's CSR with `create-certificate-from-csr`. It holds the device's public key; the private key stays on the device. The certificate is attached to the thing, and the device presents it in the TLS handshake (mutual TLS). AWS IoT can deactivate or revoke it at any time.

**Policy.** An AWS IoT policy attached to the certificate says what the device may do over MQTT. `rpi-ex-client` lets a device connect only with its own thing name as the client ID, and publish, subscribe and receive only under `rpi/<its thing name>/`. It uses [thing policy variables](https://docs.aws.amazon.com/iot/latest/developerguide/thing-policy-variables.html), so one policy serves all devices. Greengrass cores get a policy of their own, as Greengrass does not support those variables.

**Endpoint.** Your account's AWS IoT data endpoint in a Region, `<id>-ats.iot.<region>.amazonaws.com`, which every device connects to (`aws iot describe-endpoint --endpoint-type iot:Data-ATS`). Its server certificate chains to Amazon Root CA 1, which the devices trust (Raspberry Pi OS has it; the Pico firmware builds it in).

**Topics.** These examples use `rpi/<thing>/hello`, `rpi/<thing>/count` (device to cloud) and `rpi/<thing>/cmd` (cloud to device). Topics need no setting up; the policy decides who may use which.

**Greengrass.** On a Greengrass core the nucleus holds the connection, and components (such as `rpi-ex.Hello`) publish and subscribe through it over local IPC. The core also gets AWS credentials through a *role alias*: it presents its certificate to the AWS IoT credentials endpoint and receives temporary credentials for the *token-exchange role* (`rpi-ex-gg-tes`), which here allows writing logs to CloudWatch and nothing else. Components are deployed from the cloud to a *thing group* (`rpi-ex-gg-classic`, `rpi-ex-gg-lite`).

**Names.** Everything these tools create in AWS starts with `rpi-ex-` (or `rpi-ex.` for the component), so you can see it and remove it.
