# Examples we may follow with

This repository's examples stop at a device, or a small batch of devices, registered from a desk. Examples we may follow with:

- **Fleet provisioning.** For many devices, AWS IoT offers [fleet provisioning](https://docs.aws.amazon.com/iot/latest/developerguide/provision-wo-cert.html): by claim (a shared bootstrap certificate in the image, exchanged for a device certificate) or by trusted user. A claim certificate puts a shared credential in every image, which these examples avoid; a flow based on a CSR keeps the key on the device either way.
- **Factory provisioning with rpi-sb-provisioner.** [rpi-sb-provisioner](https://github.com/raspberrypi/rpi-sb-provisioner) sets up secure boot, programs OTP key 1 and writes the signed image on Compute Modules and Raspberry Pi computers in a factory. Joined with the key and CSR flow here, each device would make its key at first boot and a station would register its CSR.
- **Key rotation**, started by the cloud, with the old certificate revoked once the new one works (see [key-storage.md](key-storage.md)).
- **TrustZone on the RP2350**, so that the key is used by code on the secure side and never exposed to the application.
- **MicroPython** on the Pico 2 W.
