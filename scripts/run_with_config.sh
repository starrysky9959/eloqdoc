#!/bin/bash
#
# Script to run MongoDB with an EloqDoc configuration file
# Usage: ./run_mongo_use_config.sh <build-type> [additional mongod params]

# Exit on error, undefined variable, or pipeline failure
set -euo pipefail

# ------------- Color definitions -------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ------------- Logging functions -------------
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1" >&2
}

# ------------- Library Path Setup -------------
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}:src/mongo/db/modules/eloq/build:/usr/local/lib"
export LD_PRELOAD="/lib/libbrpc.so"

# ------------- Help message -------------
show_usage() {
    echo "Usage: $0 <build-type> [additional mongod params]"
    echo "  build-type: Debug, Release, or RelWithDebInfo"
    echo "  additional params: Any additional parameters to pass to mongod"
    echo
    echo "Example: $0 Debug --port 27018"
}

# ------------- Validate input parameters -------------
if [ $# -lt 1 ]; then
    log_error "Build type must be specified"
    show_usage
    exit 1
fi

# ------------- Check if we're in the right directory -------------
if [ ! -d "src/mongo/db/modules/eloq" ]; then
    log_error "This script must be run from the EloqDoc root directory"
    log_error "Current directory: $(pwd)"
    log_error "Please change to the EloqDoc root directory and try again"
    exit 1
fi

# ------------- Parse build type (case insensitive) -------------
BUILD_TYPE_LOWER=$(echo "$1" | tr '[:upper:]' '[:lower:]')
shift  # Remove the build type from the arguments

case "${BUILD_TYPE_LOWER}" in
    "debug")
        BUILD_TYPE="Debug"
        CONFIG_PATH="src/mongo/db/modules/eloq/config/standalone/debug_coroutine.conf"
        ;;
    "release")
        BUILD_TYPE="Release"
        CONFIG_PATH="src/mongo/db/modules/eloq/config/standalone/release_coroutine.conf"
        ;;
    "relwithdebinfo")
        BUILD_TYPE="RelWithDebInfo"
        CONFIG_PATH="src/mongo/db/modules/eloq/config/standalone/release_coroutine.conf"
        ;;
    *)
        log_error "Invalid build type: ${BUILD_TYPE_LOWER}"
        show_usage
        exit 1
        ;;
esac

# ------------- Validate executable and config file existence -------------
MONGOD_PATH="./build/${BUILD_TYPE}/mongo/mongod"
if [ ! -f "${MONGOD_PATH}" ]; then
    log_error "MongoDB executable not found at ${MONGOD_PATH}"
    log_error "Please build MongoDB with the ${BUILD_TYPE} configuration first"
    exit 1
fi

if [ ! -f "${CONFIG_PATH}" ]; then
    log_error "Configuration file not found at ${CONFIG_PATH}"
    exit 1
fi

# ------------- Run MongoDB with the configuration -------------
log_info "Starting MongoDB (${BUILD_TYPE}) with configuration: ${CONFIG_PATH}"
log_info "Additional parameters: $*"
log_info "Press Ctrl+C to stop the server"

# Execute MongoDB with config and any additional parameters
${MONGOD_PATH} --config ${CONFIG_PATH} "$@"
