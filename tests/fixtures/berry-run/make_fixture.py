#!/usr/bin/env python3
"""Generates tests/fixtures/berry-run/sample.replay.

The `berry-run replay` end-to-end check needs a real, well-formed replay
container to read. Rather than check an opaque binary blob into the tree, the
fixture is produced here from the container's own bytes (a build of the writer
emitted them), and the base64 constant below is the fixture.

Run it to regenerate the fixture. `make test` verifies the checked-in file still
parses with `berry-run replay`, so drift is caught.
"""

import base64
import os
import sys

# A three-keyframe replay: each keyframe carries a chunk record, a packet record
# and an entity-move record, so the reader exercises the LZ4 block path and the
# record histogram has more than one kind to count. Fingerprint build=4242,
# protocol=748, label "1.21.0.3".
SAMPLE_B64 = (
    "QlJQTAEAAACSEAAA7AIAAAgAAAADAAAAAwAAAAAAAABIAAAAAAAAAKQBAAAAAAAA"
    "LQYAAAAAAABUtkVVAAAAADEuMjEuMC4zMUtMQgEAAAAPAgAARQAAABXMh3YAAAAA"
    "AwAAAAAAAABkAAAAAAAAAGQAAAAAAAAAYgMAAABkAAEAUAIAAAAHCQBfLAEAAEEB"
    "AP8ZBEEBEAE8AQAJAF94AAAARAEAZASNABMECABvACgAAABFAQAPUEVFRUVFMUtM"
    "QgEAAAAPAgAAQgAAAM66psYAAAAAAwAAAAAAAAB4AAAAAAAAAHgAAAAAAAAAYgMA"
    "AAB4AAEAUAIAAAAHCQBfLAEAAEEBAP8ZBEEBEAE8ARMAUgEfRAEAZASNABME"
    "CABvACgAAABFAQAPUEVFRUVFMUtMQgEAAAAPAgAARQAAAJk00w8AAAAAAwAAAAAA"
    "AACMAAAAAAAAAIwAAAAAAAAAYgMAAACMAAEAUAIAAAAHCQBfLAEAAEEBAP8ZBEEB"
    "EAE8AQAJAF94AAAARAEAZASNABMECABvACgAAABFAQAPUEVFRUVFZAAAAAAAAABI"
    "AAAAAAAAAAMAAAAAAAAAeAAAAAAAAAC9AAAAAAAAAAMAAAAAAAAAjAAAAAAAAAAv"
    "AQAAAAAAAAMAAAAAAAAA"
)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    out_dir = os.path.join(root, "tests", "fixtures", "berry-run")
    os.makedirs(out_dir, exist_ok=True)
    data = base64.b64decode(SAMPLE_B64)
    path = os.path.join(out_dir, "sample.replay")
    with open(path, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (path, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
