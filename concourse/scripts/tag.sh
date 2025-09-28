#!/bin/bash
set -exuo pipefail

export WORKSPACE=$PWD
sudo chown -R $USER $PWD
cd $HOME
ln -s ${WORKSPACE}/eloqdoc_src eloqdoc
cd eloqdoc
ln -s $WORKSPACE/eloq_logservice_src src/mongo/db/modules/eloq/eloq_log_service
pushd src/mongo/db/modules/eloq/tx_service
ln -s $WORKSPACE/raft_host_manager_src raft_host_manager
popd

git config --global user.email "concourse@noreply.com"
git config --global user.name "concourse-ci"

# Configure git to use SSH key for authentication (parity with eloqkv)
mkdir -p ~/.ssh
echo "$GIT_SSH_KEY" > ~/.ssh/id_rsa
chmod 600 ~/.ssh/id_rsa
ssh-keyscan github.com >> ~/.ssh/known_hosts

# exit detach mode
git checkout -
git fetch --tags

latest=$(git tag --sort=-version:refname | head -n 1)

if [[ -z "$latest" ]]; then
    digits=(0 4 14)  # Initialize with 0.4.14 if no tags exist
else
    IFS='.' read -ra digits <<<"$latest"
fi

# Ensure we always have 3 parts
while [[ ${#digits[@]} -lt 3 ]]; do
    digits+=(0)
done

case "$TAG_LEVEL" in
"major")
    ((++digits[0])) && digits[1]=0 && digits[2]=0
    ;;
"minor")
    ((++digits[1])) && digits[2]=0
    ;;
"patch") ((++digits[2])) ;;
*)
    echo "invalid tag level $TAG_LEVEL"
    exit 1
    ;;
esac
newtag=$(
    IFS='.'
    echo "${digits[*]}"
)

bash scripts/git-tag.sh $newtag