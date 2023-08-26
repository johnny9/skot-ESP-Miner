import hashlib
import binascii


def double_sha256(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def merkle_root(coinbase_tx_hash, merkle_branches):
    current_hash = bytes.fromhex(coinbase_tx_hash)
    for branch in merkle_branches:
        branch_hash = bytes.fromhex(branch)
        # Concatenate the hashes and compute the double SHA-256 hash
        current_hash = double_sha256(current_hash + branch_hash)
    return current_hash.hex()


def swap_endian_words(hex_words):
    '''Swaps the endianness of a hexadecimal string of words and returns as another hexadecimal string.'''

    message = binascii.unhexlify(hex_words)
    if len(message) % 4 != 0:
        raise ValueError('Must be 4-byte word aligned')
    swapped_bytes = [message[4 * i: 4 * i + 4][::-1] for i in range(0, len(message) // 4)]
    return ''.join([binascii.hexlify(word).decode('utf-8') for word in swapped_bytes])

#print("Merkle root:", merkle_root(coinbase_tx_hash, merkle_branches))
# cd1be82132ef0d12053dcece1fa0247fcfdb61d4dbd3eb32ea9ef9b4c604a846
# last 4 bytes of merkle is C6 04 A8 46

version = b'\x20\x00\x00\x04'  # little-endian encoding of version 1
prev_block_hash = bytes.fromhex(swap_endian_words("bf44fd3513dc7b837d60e5c628b572b448d204a8000007490000000000000000"))
# merkle calculated above
merkle_root = bytes.fromhex(('cd1be82132ef0d12053dcece1fa0247fcfdb61d4dbd3eb32ea9ef9b4c604a846'))
timestamp = b'\x64\x65\x8b\xd8'  # little-endian encoding of Unix timestamp 64658bd8
difficulty_target = b'\x17\x05\xdd\x01' # little-endian encoding of difficulty 1705dd01
nonce = b'\x00\x00\x00\x00'  # example nonce value

block_header = version + prev_block_hash + merkle_root + timestamp + difficulty_target + nonce
print(block_header.hex())
#20000004bf44fd3513dc7b837d60e5c628b572b448d204a800000749000000000000000021e81bcd120def32cece3d057f24a01fd461dbcf32ebd3dbb4f99eea46a804c664658bd81705dd0100000000
#[20 00 00 04 BF 44 FD 35 13 DC 7B 83 7D 60 E5 C6 28 B5 72 B4 48 D2 04 A8 00 00 07 49 00 00 00 00 00 00 00 00 21 E8 1B CD 12 0D EF 32 CE CE 3D 05 7F 24 A0 1F D4 61 DB CF 32 EB D3 DB B4 F9 9E EA ]
# output: 0400002035fd44bf837bdc13c6e5607db472b528a804d248490700000000000000000000cd1be82132ef0d12053dcece1fa0247fcfdb61d4dbd3eb32ea9ef9b4c604a846d88b656401dd7830351700000000
