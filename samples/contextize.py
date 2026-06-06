# Copyright (c) 2025-2026 Krzysztof Badziak
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import json
import sys

def unpack_context_from_meta(meta):
    meta_val = int(meta, base=16)
    flow_id =    (meta_val & 0x00000000FFFFFFFF)
    context_id = (meta_val & 0x0000FFFF00000000) >> 32
    control_id = (meta_val & 0xFFFF000000000000) >> 48

    return control_id, context_id, flow_id

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 contextize.py <trace_file>")
        sys.exit(1)

    with open(sys.argv[1], "r") as f:
        loaded_trace_json = json.load(f)

    events = loaded_trace_json['traceEvents']
    for event in events:
        if 'args' in event:
            meta = False
            if 'b_meta' in event['args']:
                meta = event['args']['b_meta']
            elif 'e_meta' in event['args']:
                meta = event['args']['e_meta']
            elif 'flow_id' in event['args']:
                meta = event['args']['flow_id']
            
            if meta:
                control_id, context_id, _ = unpack_context_from_meta(meta)

                if control_id == 77:
                    event['pid'] = context_id
    
    # Fill meta events to translate out process names.
    events.append(json.loads("{\"name\": \"process_name\", \"ph\": \"M\", \"pid\": 0, \"args\": {\"name\" : \"client\"}}"))
    events.append(json.loads("{\"name\": \"process_name\", \"ph\": \"M\", \"pid\": 1, \"args\": {\"name\" : \"server\"}}"))
    events.append(json.loads("{\"name\": \"process_name\", \"ph\": \"M\", \"pid\": 2, \"args\": {\"name\" : \"io\"}}"))
    events.append(json.loads("{\"name\": \"process_name\", \"ph\": \"M\", \"pid\": 3, \"args\": {\"name\" : \"network\"}}"))     

    with open("contextized_" + sys.argv[1], "w") as f:
        json.dump(loaded_trace_json, f)

if __name__ == "__main__":
    main()
