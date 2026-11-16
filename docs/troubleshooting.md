# Troubleshooting

**Start here.** AWS IoT Core can log why it refused a connection or a message. Turn on logging once per Region (it writes to CloudWatch Logs, group `AWSIotLogsV2`; use `INFO` or `ERROR`, as `DEBUG` costs more):

```sh
aws iot set-v2-logging-options --default-log-level ERROR --role-arn <a role AWS IoT may use to write logs>
```

or in the console: **AWS IoT** → **Settings** → **Logs**.

## The device does not connect

| Symptom | Likely cause | Fix |
|---|---|---|
| TLS handshake fails at once, "certificate not yet valid" or "expired" | The clock is wrong (no network time yet, or none on a Pico) | Raspberry Pi: `timedatectl` shows `System clock synchronized: yes`. Pico: the firmware waits for SNTP; check UDP port 123 is open |
| TLS fails: unknown CA, or the server name does not match | Wrong endpoint (another Region or account), or a missing root CA | Use the endpoint from `aws iot describe-endpoint --endpoint-type iot:Data-ATS` in the same Region as the thing. Raspberry Pi OS: `ls /etc/ssl/certs/Amazon_Root_CA_1.pem` |
| TLS fails after the server's certificate: "bad certificate" or the connection is closed | The device's certificate is inactive, revoked, or not from this account and Region; or the key does not match the certificate | `aws iot describe-certificate --certificate-id <id>`: status `ACTIVE`. `rpi-aws-keygen --install` and the Pico's `SAVE` check the key matches |
| TLS works, MQTT CONNECT refused | The policy is missing, or the client ID is not the thing name, or the certificate is not attached to the thing | `aws iot list-attached-policies --target <certificate ARN>`; `aws iot list-thing-principals --thing-name <thing>` |
| Disconnected again and again | Two devices (or two processes) with the same client ID | One connection per client ID: stop the other (for example the client service while testing by hand) |
| Pico: `wifi: failed, retrying` | Wrong SSID or password, or a WPA3-only network | The firmware uses WPA2 (AES); send the right settings with `pico/tools/pico-enrol.py wifi --port …` |
| Raspberry Pi: `rpi-aws-keygen --store wrapped` says OTP key 1 is not programmed | The board has no device key in OTP | `sudo rpi-aws-otp-init` explains the options; the write is permanent |
| `/run/rpi-aws-examples/device.key is missing` | The wrapped key was not unwrapped at boot | `systemctl status rpi-aws-keyunwrap`; `sudo rpi-aws-keyhelper unwrap` |

## Messages do not arrive

- **Policy denials do not show on the device**: a publish to a topic the policy does not allow is dropped (MQTT 3.1.1), or the connection is closed. Check the AWS IoT logs above for `AUTHORIZATION_FAILURE`.
- **Subscribe in the same Region** in the MQTT test client as the device.
- **Topic filters**: `rpi/#` matches everything under `rpi/`; `rpi/+/count` matches the counts of all devices.

## Greengrass

| Symptom | Where to look |
|---|---|
| The nucleus does not connect | Classic: `sudo tail -f /greengrass/v2/logs/greengrass.log`. Lite: `journalctl -u ggl.core.iotcored` |
| Deployment `FAILED` | `aws greengrassv2 list-effective-deployments --core-device-thing-name <thing>`; on the device, Classic's `greengrass.log`, Lite's `journalctl -u ggl.core.ggdeploymentd` |
| Lite: "doesn't claim platform … compatibility" | The recipe's platform needs `runtime: "*"` (or `aws_nucleus_lite`) |
| Lite: "Recipes without at least 1 valid lifecycle step" | Lite reads recipe keys case-sensitively: `install`, `run` |
| The component runs but publishing fails with "Not Authorized" | Its `accessControl` does not cover the topic. A changed default applies only if the deployment resets the component's configuration (`gg-deploy-hello` does) |
| No logs in CloudWatch | Classic: `aws.greengrass.LogManager` uploads about once a minute; see `/greengrass/v2/logs/aws.greengrass.LogManager.log`. Lite: `journalctl -u ggl.aws.greengrass.SystemLogForwarder`. Both need the token-exchange role's CloudWatch Logs permissions |
| AWS credentials fail (token exchange) | The core's policy must allow `iot:AssumeRoleWithCertificate` on the role alias, and the role must trust `credentials.iot.amazonaws.com` |
