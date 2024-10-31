# Copyright 2018- The Pixie Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

import os
import subprocess
import argparse
import hashlib
import pandas as pd
from security import safe_command

# For extracting fields from Tshark output.
kTCPPayloadIndex = 0
kUDPPayloadIndex = 1
kProtocolIndex = 2
kIPSrcIndex = 3
kIPDstIndex = 4
kTCPSrcPortIndex = 5
kUDPSrcPortIndex = 6
kTCPDstPortIndex = 7
kUDPDstPortIndex = 8


# For parsing out packets from TCP payload.
kMySQLExtraLength = 4
kPgSQLExtraLength = 1
kAMQPExtraLength = 8

# For specifying fields in Tshark 2nd pass analysis.
kMySQLLengthField = "packet_length"
kPgSQLLengthField = "length"
kAMQPLengthField = "length"

# Protocols not specified in ProtocolParsingSpecs are not split by packet
# lengths, and the entire TCP payload is treated as one packet.
ProtocolParsingSpecs = {
    "mysql": {"length_field": kMySQLLengthField,
              "extra_length": kMySQLExtraLength},
    "pgsql": {"length_field": kPgSQLLengthField,
              "extra_length": kPgSQLExtraLength},

    # Advanced Message Queueing Protocol, used in RabbitMQ.
    "amqp": {"length_field": kAMQPLengthField,
             "extra_length": kAMQPExtraLength}
}

# TODO(chengruizhe): Add unit tests for dataset_generation.


class DuplicateChecker:
    def __init__(self):
        self.bank = set()
        self.hash_fn = hashlib.md5()

    def check_duplicate(self, payload):
        self.hash_fn.update(payload.encode('utf-8'))
        hash = self.hash_fn.hexdigest()
        if hash in self.bank:
            return True
        else:
            self.bank.add(hash)
            return False


def gen_tshark_cmd():
    """
    Generate command to do second pass analysis of pcap files.
    """
    tshark_cmd = "tshark -2 -r {pcap_file} -T fields \
    -e tcp.payload \
    -e udp.payload \
    -e frame.protocols \
    -e ip.src \
    -e ip.dst \
    -e tcp.srcport \
    -e udp.srcport \
    -e tcp.dstport \
    -e udp.dstport \
    -d tcp.port==27017,mongo"

    for protocol, spec in ProtocolParsingSpecs.items():
        field_name = spec["length_field"]
        tshark_cmd += f" -e {protocol}.{field_name}"
    tshark_cmd += " > {output_file}"
    return tshark_cmd


def run_tshark(pcap_path, cmd):
    output_file = pcap_path[:-6] + "txt"
    tshark_cmd = cmd.format(pcap_file=pcap_path, output_file=output_file)
    safe_command.run(subprocess.run, tshark_cmd, shell=True, capture_output=False)
    return output_file


def split_by_length(payload, packet_lengths, protocol):
    """
    Breaks up payload into packets (for protocols such as MySQL and AMQP).
    :param payload: payload string to be broken up
    :param packet_lengths: lengths of the packets in bytes
    :param protocol: how we peel off packets depends on the protocol
    :return: A list of strings
    """
    assert protocol in ProtocolParsingSpecs, f"Protocol {protocol} not in ParsingSpecs."
    extra_len = ProtocolParsingSpecs[protocol]["extra_length"]

    packets = []
    offset = 0
    for length in packet_lengths:
        if offset >= len(payload):
            return packets

        # Convert from number of bytes to number of hex chars.
        packet_num_hex = (length + extra_len) * 2
        packets.append(payload[offset:offset + packet_num_hex])
        offset += packet_num_hex
    return packets


def parse_protocol(frame_protocol):
    """
    Convert protocol identified by Tshark to a predefined protocol.
    :param frame_protocol: Example: sll:ethertype:ip:tcp:http:http2:grpc
    :return: Example: http2
    """
    # Putting http after http2/grpc since it can overlap with http2 and grpc.
    protocols = ["http2", "mysql", "pgsql", "cql", "amqp", "redis", "dns", "mongo", "http", "ssh",
                 "kafka", "mux", "tls"]
    for protocol in protocols:
        if protocol in frame_protocol:
            return protocol
    else:
        return "unknown"


def get_packet_lengths_field(line):
    """
    Parse out the packet lengths field at the end of a line. Returns none if the protocol
    doesn't have the field.
    """
    for field in line[kUDPDstPortIndex + 1:]:
        if field:
            return [int(length) for length in field.split(",")]
    return None


def gen_tsv(input_path, output_path):
    """
    For each row of the input file, parse out tcp/udp payload, src and dst ports, protocol,
    and break up packets. If the packet is not a duplicate, write it to the output tsv file.
    :param input_path: the txt file generated by the second pass of tshark.
    :param output_path: the tsv file to write the results.
    """
    assert os.path.exists(input_path)

    duplicate_checker = DuplicateChecker()
    with open(input_path, "r") as in_file, open(output_path, "w") as out_file:
        for line in in_file.readlines():
            # Format: tcp_payload, udp_payload, protocol, tcp_src, udp_src, tcp_dst, udp_dst,
            # packet_lengths
            line = line.split("\t")
            if line[-1] == "\n":
                line = line[:-1]

            # If tcp is not in protocol, defaults to udp
            is_tcp = "tcp" in line[kProtocolIndex]
            protocol = parse_protocol(line[kProtocolIndex])

            if protocol == "unknown":
                continue

            src_addr = line[kIPSrcIndex]
            dst_addr = line[kIPDstIndex]

            if is_tcp:
                payload = line[kTCPPayloadIndex]
                src_port = int(line[kTCPSrcPortIndex])
                dst_port = int(line[kTCPDstPortIndex])
            else:
                payload = line[kUDPPayloadIndex]
                src_port = int(line[kUDPSrcPortIndex])
                dst_port = int(line[kUDPDstPortIndex])

            packet_lengths = get_packet_lengths_field(line)
            if packet_lengths:
                packets = split_by_length(payload, packet_lengths, protocol)
            else:
                packets = [payload]

            for p in packets:
                if not duplicate_checker.check_duplicate(p):
                    row = f"{p}\t{protocol}\t{src_addr}\t{dst_addr}\t{src_port}\t{dst_port}\n"
                    out_file.write(row)


def get_tsv_paths(data_root):
    """
    Returns the paths to all the tsv files in the dataset.
    """
    results = []

    for capture_run in os.listdir(data_root):
        capture_path = os.path.join(data_root, capture_run)
        if not os.path.isdir(capture_path):
            continue
        for pod_name in os.listdir(capture_path):
            if not os.path.isdir(os.path.join(capture_path, pod_name)):
                continue
            tsv_files = [f for f in os.listdir(os.path.join(capture_path, pod_name))
                         if f.endswith(".tsv")]
            for tsv in tsv_files:
                results.append(os.path.join(capture_path, pod_name, tsv))
    return results


def gen_conn_tsv(tsv_file, out_file):
    """
    Does a groupby on packet level data and aggregate to output connection level data.
    """
    df = pd.read_csv(tsv_file, delimiter='\t',
                     names=["payload", "protocol", "src_addr", "dst_addr", "src_port", "dst_port"])
    conn_data = df.groupby(["protocol", "src_addr", "dst_addr", "src_port", "dst_port"],
                           as_index=False).agg({"payload": lambda x: ",".join(x)})
    conn_data.to_csv(out_file, sep="\t",
                     columns=["payload", "protocol", "src_addr", "dst_addr", "src_port",
                              "dst_port"], header=False, index=False)


def gen_bidirectional_tsv(tsv_file, out_file):
    """
    Make the packets direction agnostic and group them to output bi-directional connection data.
    """
    df = pd.read_csv(tsv_file, delimiter='\t',
                     names=["payload", "protocol", "src_addr", "dst_addr", "src_port", "dst_port"])
    if df.empty:
        return

    def merge_addr_port(row):
        src = str(row["src_addr"]) + ":" + str(row["src_port"])
        dst = str(row["dst_addr"]) + ":" + str(row["dst_port"])
        if src > dst:
            return src + " " + dst
        else:
            return dst + " " + src

    # Map addr and port to a direction-agnostic string
    df["bidir_conn"] = df.apply(merge_addr_port, axis=1)
    bidir_conn_data = df.groupby(["protocol", "bidir_conn"],
                                 as_index=False).agg({"payload": lambda x: ",".join(x)})
    bidir_conn_data.to_csv(out_file, sep="\t",
                           columns=["payload", "protocol"], header=False, index=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Protocol Dataset Generation")
    parser.add_argument("--dataset", required=True, help="Path to the dataset")
    args = parser.parse_args()

    tshark_cmd = gen_tshark_cmd()
    print(tshark_cmd)

    # Input format:
    # --dataset
    #     --Captures1
    #         --pod1
    #             --1234.pcapng
    #         --pod2
    #             --2345.pcapng
    #     --Captures2
    #         --pod1
    # Generate a tsv file containing data in each pod folder.
    for capture_run in os.listdir(args.dataset):
        capture_path = os.path.join(args.dataset, capture_run)
        if not os.path.isdir(capture_path):
            continue
        print("Processing capture: ", capture_run)
        for pod_name in os.listdir(capture_path):
            print("Processing pod: ", pod_name)
            if not os.path.isdir(os.path.join(capture_path, pod_name)):
                continue
            pcap_files = [f for f in os.listdir(os.path.join(capture_path, pod_name))
                          if f.endswith(".pcapng")]
            for pcap in pcap_files:
                pcap_path = os.path.join(capture_path, pod_name, pcap)
                tshark_output = run_tshark(pcap_path, tshark_cmd)
                tsv_output = tshark_output[:-3] + "tsv"
                gen_tsv(input_path=tshark_output, output_path=tsv_output)

    tsv_paths = get_tsv_paths(args.dataset)

    # Consolidate the tsv files into a big tsv with each row representing a packet.
    with open(os.path.join(args.dataset, "packet_dataset.tsv"), "a+") as out_file:
        for tsv_path in tsv_paths:
            with open(tsv_path) as tsv_file:
                out_file.write(tsv_file.read())

    # Consolidate the tsv files into a big tsv with connection level data. This
    # enables building models taking multiple packets in a data stream.
    with open(os.path.join(args.dataset, "conn_dataset.tsv"), "a+") as out_file:
        for tsv_path in tsv_paths:
            with open(tsv_path) as tsv_file:
                gen_conn_tsv(tsv_file, out_file)

    # Consolidate the tsv files into a big tsv with bi-direction connection level data.
    # Request and response packets are grouped into one bi-directional connection.
    with open(os.path.join(args.dataset, "bi_dir_conn_dataset.tsv"), "a+") as out_file:
        for tsv_path in tsv_paths:
            with open(tsv_path) as tsv_file:
                gen_bidirectional_tsv(tsv_file, out_file)
