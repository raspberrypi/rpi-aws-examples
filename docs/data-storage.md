# Where to keep the data

Messages that reach AWS IoT Core are not stored anywhere by default. To keep them, add an [AWS IoT rule](https://docs.aws.amazon.com/iot/latest/developerguide/iot-rules.html): a SQL-like query over a topic filter (for example `SELECT * FROM 'rpi/+/count'`) with an action that sends the matching messages on. The usual destinations:

| Destination | Rule action | Good for | Keep in mind |
|---|---|---|---|
| **Amazon S3**, through Amazon Data Firehose | `firehose` | Cheap long-term storage of everything; analysis later with Athena, Glue or your own tools | Firehose batches (by size or every few minutes), so data is not instant. Choose a partitioning scheme (by date, by device) that suits your queries |
| **Amazon S3 Tables** (Apache Iceberg) and **Athena** | `firehose` (to an S3 table) | SQL over large histories, with schema evolution and compaction handled for you | Newer; more set-up than plain S3 |
| **Amazon DynamoDB** | `dynamoDBv2` | The latest state of each device, or lookups by device and time; fast reads for an app | Pay per request and per GB stored; design the key (device ID and timestamp) for your queries; not for analytics |
| **AWS IoT SiteWise** | `iotSiteWise` | Industrial assets and their measurements, models and dashboards | Most structure to set up; suited to plants and equipment rather than a few devices |
| **Amazon Aurora** (PostgreSQL or MySQL) | `sqs`, then a Lambda function that writes in batches | Readings next to your other relational data (devices, customers, sites), with joins and transactions | No direct rule action: route through SQS (see [below](#relational-and-time-series-databases-through-sqs)). Runs in a VPC |
| **TimescaleDB** (PostgreSQL with time-series extensions) | `sqs`, then a Lambda function that writes in batches | Large volumes of timestamped readings: compression, downsampling and retention built in, queried with SQL | Not offered on RDS or Aurora: run it on EC2 or use Tiger Cloud (the hosted service, on AWS). Route through SQS as for Aurora |

Some general points:

- Put a **timestamp** in each message (or add one in the rule with `timestamp()`), and the thing name (`topic(2)` in the rule for `rpi/<thing>/...`).
- Send rule failures to an **error action** (for example to S3 or CloudWatch Logs), so that a misconfigured destination does not drop data silently.
- The rule needs an **IAM role** that allows writing to the destination, and nothing more.
- Start with S3 through Firehose if you are not sure: it is cheap, keeps everything, and you can load the data elsewhere later.

## Relational and time-series databases: through SQS

A database that takes SQL connections should not get one write per message. A rule that calls a Lambda function for each message opens connections as fast as devices publish, and a database restart or a maintenance window drops data. Put an **Amazon SQS** queue in between:

```
devices → IoT rule (sqs action) → SQS queue → Lambda (batch) → Aurora or TimescaleDB
```

- **Batching.** The Lambda event source mapping for SQS hands the function up to 10,000 messages at a time, waiting up to 5 minutes to fill a batch (`BatchSize`, `MaximumBatchingWindowInSeconds`). One multi-row `INSERT` per batch, in one transaction, costs far less than one per message.
- **Buffering.** The queue keeps messages (4 days by default, up to 14) while the database is busy, being patched or down; nothing is lost, and the function catches up.
- **Retries.** A failed batch goes back on the queue. Turn on `ReportBatchItemFailures` so that only the failed messages are retried, and give the queue a **dead-letter queue** for messages that never succeed.
- **Connections.** Cap the function's concurrency (`MaximumConcurrency` on the event source mapping) so that it cannot exhaust the database's connections. For Aurora, use **RDS Proxy** or the **RDS Data API** instead of a connection per invocation.
- **Order.** A standard queue does not keep order; write the device's own timestamp, not the time of arrival. A FIFO queue keeps order per group (the thing name, say), at a lower throughput.

Which to choose:

- **Aurora** when the readings belong with relational data you already keep: devices, owners, sites, alarms. You get joins, transactions and the PostgreSQL or MySQL you know, fully managed. Aurora Serverless v2 scales with the load and can pause when idle. For years of high-rate readings it gets big and slow to query without your own partitioning and roll-ups.
- **TimescaleDB** when the data is mainly time series and there is a lot of it. It is still PostgreSQL (the same SQL, joins and drivers), but tables are partitioned by time automatically (hypertables), old data is compressed (often ten times or more), continuous aggregates keep hourly or daily roll-ups up to date, and retention policies drop raw data after a set age. The cost is running it yourself on EC2, or a second vendor (Tiger Cloud).
- If you are not sure, start with S3 through Firehose as well: it keeps everything cheaply, and you can load it into either later.

These examples deploy no rules or storage.
