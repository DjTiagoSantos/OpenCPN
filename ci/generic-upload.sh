#!/bin/sh

expand() { for arg in "$@"; do test -f $arg && echo $arg; done }

REPO="alec-leamas/opencpn"

test -z "$TRAVIS_BUILD_DIR" || cd $TRAVIS_BUILD_DIR
cd build

case "$OCPN_TARGET" in
    xenial|trusty) 
        for src in $(expand *.deb); do
            old=$(basename $src)
            new=$(echo $old | sed "s/opencpn/opencpn-${OCPN_TARGET}/")
            sudo mv $old $new
            echo "Renaming $old to $new"
        done
        ;;
    mingw)
        for src in $(expand *setup.exe); do
            old=$(basename $src)
            new=$(echo $old | sed "s/opencpn/opencpn-mingw/")
            sudo mv $old $new
            echo "Renaming $old to $new"
        done
        ;;
esac

if [ -n "$TRAVIS_BUILD_NR" ]; then
    BUILD_NR="$TRAVIS_BUILD_NR"
elif [ -n "$CIRCLE_BUILD_NUM" ]; then
    BUILD_NR="$CIRCLE_BUILD_NUM"
else
    BUILD_NR="1"
fi

if [ -z "$CLOUDSMITH_API_KEY" ]; then
    echo 'Cannot deploy to cloudsmith: missing $CLOUDSMITH_API_KEY'
else
    echo 'Deploying to cloudsmith'
    sudo python3 -m pip install -q cloudsmith-cli
    for src in $(expand *.dmg *setup.exe *.deb); do
        set -x
        old=$(basename $src)
        new=$(echo $old | sed "s/+/+${BUILD_NR}./")
        if [ "$old" != "$new" ]; then sudo mv "$old" "$new"; fi
        cloudsmith push raw --republish --no-wait-for-sync $REPO $new
        set +x
    done
fi

exit 0

# TODO: Upload .pdb and .lib files as well.

