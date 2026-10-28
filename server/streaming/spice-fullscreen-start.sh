#!/bin/sh
set -eu
cd /
test -r /etc/default/spice-fullscreen
. /etc/default/spice-fullscreen
: "${DESKTOP_UID:?}" "${DESKTOP_GID:?}" "${VIDEO_GID:?}" "${RENDER_GID:?}" "${XAUTHORITY_FILE:?}"
port=/dev/virtio-ports/org.spice-space.stream.0
auth="$XAUTHORITY_FILE"
for attempt in $(seq 1 60); do
    if [ -c "$port" ] && [ -f "$auth" ] && [ -S /tmp/.X11-unix/X0 ]; then
        break
    fi
    sleep 1
done
test -c "$port"
test -f "$auth"
test -S /tmp/.X11-unix/X0
exec /usr/bin/docker run --rm --name spice-fullscreen-h264 \
    --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
    --user "$DESKTOP_UID:$DESKTOP_GID" --group-add "$VIDEO_GID" --group-add "$RENDER_GID" \
    --tmpfs /tmp:rw,nosuid,nodev,size=64m \
    --device "$port:$port" --device /dev/dri \
    --mount type=bind,src=/tmp/.X11-unix,dst=/tmp/.X11-unix,readonly \
    --mount "type=bind,src=$auth,dst=/run/spice-xauthority,readonly" \
    local/spice-fullscreen-h264:0.3-dx2
