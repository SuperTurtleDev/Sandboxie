/*
 * Copyright (C) 2016 wj32
 * Copyright (C) 2021-2025 David Xanatos, xanasoft.com
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "driver.h"
#include "util.h"

#include <bcrypt.h>

#ifdef __BCRYPT_H__
#define KPH_SIGN_ALGORITHM BCRYPT_ECDSA_P256_ALGORITHM
#define KPH_SIGN_ALGORITHM_BITS 256
#define KPH_HASH_ALGORITHM BCRYPT_SHA256_ALGORITHM
#define KPH_BLOB_PUBLIC BCRYPT_ECCPUBLIC_BLOB
#endif

#define KPH_SIGNATURE_MAX_SIZE (128 * 1024) // 128 kB

static UCHAR KphpTrustedPublicKey[] =
{
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0x05, 0x7A, 0x12, 0x5A, 0xF8, 0x54, 0x01, 0x42,
    0xDB, 0x19, 0x87, 0xFC, 0xC4, 0xE3, 0xD3, 0x8D, 0x46, 0x7B, 0x74, 0x01, 0x12, 0xFC, 0x78, 0xEB,
    0xEF, 0x7F, 0xF6, 0xAF, 0x4D, 0x9A, 0x3A, 0xF6, 0x64, 0x90, 0xDB, 0xE3, 0x48, 0xAB, 0x3E, 0xA7,
    0x2F, 0xC1, 0x18, 0x32, 0xBD, 0x23, 0x02, 0x9D, 0x3F, 0xF3, 0x27, 0x86, 0x71, 0x45, 0x26, 0x14,
    0x14, 0xF5, 0x19, 0xAA, 0x2D, 0xEE, 0x50, 0x10
};


typedef struct {
    BCRYPT_ALG_HANDLE algHandle;
    BCRYPT_HASH_HANDLE handle;
    PVOID object;
} MY_HASH_OBJ;

VOID MyFreeHash(MY_HASH_OBJ* pHashObj)
{
    if (pHashObj->handle)
        BCryptDestroyHash(pHashObj->handle);
    if (pHashObj->object)
        ExFreePoolWithTag(pHashObj->object, 'vhpK');
    if (pHashObj->algHandle)
        BCryptCloseAlgorithmProvider(pHashObj->algHandle, 0);
}

NTSTATUS MyInitHash(MY_HASH_OBJ* pHashObj)
{
    NTSTATUS status;
    ULONG hashObjectSize;
    ULONG querySize;
    memset(pHashObj, 0, sizeof(MY_HASH_OBJ));

    if (!NT_SUCCESS(status = BCryptOpenAlgorithmProvider(&pHashObj->algHandle, KPH_HASH_ALGORITHM, NULL, 0)))
        goto CleanupExit;

    if (!NT_SUCCESS(status = BCryptGetProperty(pHashObj->algHandle, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjectSize, sizeof(ULONG), &querySize, 0)))
        goto CleanupExit;

    pHashObj->object = ExAllocatePoolWithTag(PagedPool, hashObjectSize, 'vhpK');
    if (!pHashObj->object) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto CleanupExit;
    }

    if (!NT_SUCCESS(status = BCryptCreateHash(pHashObj->algHandle, &pHashObj->handle, (PUCHAR)pHashObj->object, hashObjectSize, NULL, 0, 0)))
        goto CleanupExit;

CleanupExit:
    // on failure the caller must call MyFreeHash

    return status;
}

NTSTATUS MyHashData(MY_HASH_OBJ* pHashObj, PVOID Data, ULONG DataSize)
{
    return BCryptHashData(pHashObj->handle, (PUCHAR)Data, DataSize, 0);
}

NTSTATUS MyFinishHash(MY_HASH_OBJ* pHashObj, PVOID* Hash, PULONG HashSize)
{
    NTSTATUS status;
    ULONG querySize;

    if (!NT_SUCCESS(status = BCryptGetProperty(pHashObj->algHandle, BCRYPT_HASH_LENGTH, (PUCHAR)HashSize, sizeof(ULONG), &querySize, 0)))
        goto CleanupExit;

    *Hash = ExAllocatePoolWithTag(PagedPool, *HashSize, 'vhpK');
    if (!*Hash) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto CleanupExit;
    }

    if (!NT_SUCCESS(status = BCryptFinishHash(pHashObj->handle, (PUCHAR)*Hash, *HashSize, 0)))
        goto CleanupExit;

    return STATUS_SUCCESS;

CleanupExit:
    if (*Hash) {
        ExFreePoolWithTag(*Hash, 'vhpK');
        *Hash = NULL;
    }

    return status;
}

NTSTATUS KphVerifySignature(
    _In_ PVOID Hash,
    _In_ ULONG HashSize,
    _In_ PUCHAR Signature,
    _In_ ULONG SignatureSize
    )
{
    NTSTATUS status;
    BCRYPT_ALG_HANDLE signAlgHandle = NULL;
    BCRYPT_KEY_HANDLE keyHandle = NULL;
    PVOID hash = NULL;
    ULONG hashSize;

    // Import the trusted public key.

    if (!NT_SUCCESS(status = BCryptOpenAlgorithmProvider(&signAlgHandle, KPH_SIGN_ALGORITHM, NULL, 0)))
        goto CleanupExit;
    if (!NT_SUCCESS(status = BCryptImportKeyPair(signAlgHandle, NULL, KPH_BLOB_PUBLIC, &keyHandle,
        KphpTrustedPublicKey, sizeof(KphpTrustedPublicKey), 0)))
    {
        goto CleanupExit;
    }

    // Verify the hash.

    if (!NT_SUCCESS(status = BCryptVerifySignature(keyHandle, NULL, Hash, HashSize, Signature,
        SignatureSize, 0)))
    {
        goto CleanupExit;
    }

CleanupExit:
    if (keyHandle)
        BCryptDestroyKey(keyHandle);
    if (signAlgHandle)
        BCryptCloseAlgorithmProvider(signAlgHandle, 0);

    return status;
}

NTSTATUS KphVerifyBuffer(
    _In_ PUCHAR Buffer,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Signature,
    _In_ ULONG SignatureSize
    )
{
    NTSTATUS status;
    MY_HASH_OBJ hashObj;
    PVOID hash = NULL;
    ULONG hashSize;

    // Hash the Buffer.

    if(!NT_SUCCESS(status = MyInitHash(&hashObj)))
        goto CleanupExit;

    MyHashData(&hashObj, Buffer, BufferSize);

	if(!NT_SUCCESS(status = MyFinishHash(&hashObj, &hash, &hashSize)))
        goto CleanupExit;

    // Verify the hash.

    if (!NT_SUCCESS(status = KphVerifySignature(hash, hashSize, Signature, SignatureSize)))
    {
        goto CleanupExit;
    }

CleanupExit:

    if (hash)
        ExFreePoolWithTag(hash, 'vhpK');

    MyFreeHash(&hashObj);

    return status;
}
