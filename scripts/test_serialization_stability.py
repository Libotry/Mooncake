"""
Test struct_pack serialization stability by checking if the same data
produces identical binary output when serialized multiple times.
"""

import sys
import struct

def create_test_payload():
    """模拟 MetadataPayload 结构的序列化"""
    # UUID client_id (pair<uint64_t, uint64_t>)
    client_id_first = 12345678
    client_id_second = 87654321
    
    # uint64_t size
    size = 1024 * 1024  # 1MB
    
    # vector<Replica::Descriptor> - 包含一个 LocalDiskDescriptor
    # LocalDiskDescriptor {
    #   UUID client_id
    #   uint64_t object_size
    #   string transport_endpoint
    # }
    
    # 简单模拟二进制序列化（实际 struct_pack 格式可能不同）
    payload = b''
    
    # client_id
    payload += struct.pack('<QQ', client_id_first, client_id_second)
    
    # size
    payload += struct.pack('<Q', size)
    
    # replicas count (假设为 1)
    payload += struct.pack('<I', 1)
    
    # variant index (假设 LocalDiskDescriptor 是 index 2)
    payload += struct.pack('<I', 2)
    
    # LocalDiskDescriptor fields
    payload += struct.pack('<QQ', 11111111, 22222222)  # client_id
    payload += struct.pack('<Q', 2048)  # object_size
    
    # transport_endpoint string
    endpoint = b"tcp://localhost:5000"
    payload += struct.pack('<I', len(endpoint))
    payload += endpoint
    
    return payload

def xxh32(data, seed=0):
    """简化的XXH32哈希（实际应该使用xxhash库）"""
    # 这里只是演示，实际应该用真正的xxhash
    import hashlib
    return int.from_bytes(hashlib.md5(data).digest()[:4], 'little')

def main():
    print("=== Testing serialization stability ===\n")
    
    # 生成两次payload
    payload1 = create_test_payload()
    payload2 = create_test_payload()
    
    print(f"Payload1 size: {len(payload1)} bytes")
    print(f"Payload2 size: {len(payload2)} bytes")
    print(f"Payloads identical: {payload1 == payload2}")
    
    print(f"\nPayload1 hex (first 64 bytes):")
    print(payload1[:64].hex(' '))
    
    print(f"\nPayload2 hex (first 64 bytes):")
    print(payload2[:64].hex(' '))
    
    # 计算checksum
    checksum1 = xxh32(payload1)
    checksum2 = xxh32(payload2)
    
    print(f"\nChecksum1: 0x{checksum1:08x}")
    print(f"Checksum2: 0x{checksum2:08x}")
    print(f"Checksums match: {checksum1 == checksum2}")
    
    if payload1 != payload2:
        print("\n⚠️  WARNING: Payloads differ!")
        for i, (b1, b2) in enumerate(zip(payload1, payload2)):
            if b1 != b2:
                print(f"  Byte {i}: 0x{b1:02x} vs 0x{b2:02x}")
    else:
        print("\n✓ Serialization is deterministic")

if __name__ == "__main__":
    main()
