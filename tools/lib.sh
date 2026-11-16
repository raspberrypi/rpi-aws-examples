# Shared by the tools in this directory. Sourced, not run.
# shellcheck shell=bash

# Every AWS object these tools create starts with this, so that you can list them and tear them down.
PREFIX="rpi-ex-"
# shellcheck disable=SC2034 # used by the scripts that source this
CLIENT_POLICY="${PREFIX}client"
# shellcheck disable=SC2034
GG_ROLE="${PREFIX}gg-tes"
# shellcheck disable=SC2034
GG_ALIAS="${PREFIX}gg-tes-alias"
# shellcheck disable=SC2034
HELLO="rpi-ex.Hello"
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "error: $*" >&2; exit 1; }
say() { echo "$*" >&2; }

# aws, with --region when one was given (otherwise the CLI's own configuration decides).
region=""
aws_() { aws ${region:+--region "$region"} "$@"; }

need() {
  command -v aws >/dev/null || die "the AWS CLI (aws) is not installed"
  command -v openssl >/dev/null || die "openssl is not installed"
}

valid_thing() {
  [[ "$1" =~ ^${PREFIX}[A-Za-z0-9_-]{1,100}$ ]] || die "thing name '$1' must be ${PREFIX} followed by letters, digits, '-' or '_'"
}

# The account and Region the CLI is using, for policy templates.
account_id() { aws_ sts get-caller-identity --query Account --output text; }
# Same order as the CLI: --region, then AWS_REGION and AWS_DEFAULT_REGION, then the profile's region.
current_region() {
  local r=${region:-${AWS_REGION:-${AWS_DEFAULT_REGION:-}}}
  [ -n "$r" ] || r=$(aws configure get region) || die "no Region: pass --region"
  echo "$r"
}

# A policy template from policies/, with its REGION, ACCOUNT and THING placeholders filled in.
render_policy() {  # template thing
  sed -e "s/REGION/$(current_region)/g" -e "s/ACCOUNT/$(account_id)/g" -e "s/THING/${2:-}/g" \
    "$TOOLS_DIR/policies/$1"
}

# Create the policy unless it exists. An existing policy is left as it is.
ensure_policy() {  # name template [thing]
  if aws_ iot get-policy --policy-name "$1" >/dev/null 2>&1; then
    say "policy $1: exists"
  else
    aws_ iot create-policy --policy-name "$1" --policy-document "$(render_policy "$2" "${3:-}")" >/dev/null
    say "policy $1: created"
  fi
}

# Delete one of our policies once nothing is attached to it.
delete_policy_if_unused() {  # name
  [[ "$1" == "$PREFIX"* ]] || return 0
  aws_ iot get-policy --policy-name "$1" >/dev/null 2>&1 || return 0
  if [ -n "$(aws_ iot list-targets-for-policy --policy-name "$1" --query 'targets[]' --output text)" ]; then
    say "policy $1: still attached elsewhere, kept"
    return 0
  fi
  local v
  for v in $(aws_ iot list-policy-versions --policy-name "$1" \
               --query 'policyVersions[?!isDefaultVersion].versionId' --output text); do
    aws_ iot delete-policy-version --policy-name "$1" --policy-version-id "$v"
  done
  aws_ iot delete-policy --policy-name "$1"
  say "policy $1: deleted"
}

# The certificate ARNs attached to a thing.
thing_certs() { aws_ iot list-thing-principals --thing-name "$1" --query 'principals[]' --output text 2>/dev/null | tr '\t' '\n' | grep ':cert/' || true; }

# SHA-256 of a public key (DER), from a CSR or a certificate in PEM.
pubkey_hash_csr() { openssl req -in "$1" -noout -pubkey | openssl pkey -pubin -outform DER | sha256sum | cut -d' ' -f1; }
pubkey_hash_cert() { openssl x509 -noout -pubkey | openssl pkey -pubin -outform DER | sha256sum | cut -d' ' -f1; }
