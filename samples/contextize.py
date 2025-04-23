import json
import sys

def unpack_meta_string(meta):
    meta_val = int(meta, base=16)
    context_id = (meta_val & 0x00000000FFFFFFFF)
    control_id = (meta_val & 0xFFFFFFFF00000000) >> 32

    return control_id, context_id

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
            
            if meta:
                control_id, context_id = unpack_meta_string(meta)

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
