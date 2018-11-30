/**
 * @file tls13_misc.c
 * @brief TLS 1.3 helper functions
 *
 * @section License
 *
 * Copyright (C) 2010-2018 Oryx Embedded SARL. All rights reserved.
 *
 * This file is part of CycloneSSL Open.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * @author Oryx Embedded SARL (www.oryx-embedded.com)
 * @version 1.9.0
 **/

//Switch to the appropriate trace level
#define TRACE_LEVEL TLS_TRACE_LEVEL

//Dependencies
#include <string.h>
#include "tls.h"
#include "tls_extensions.h"
#include "tls_certificate.h"
#include "tls_signature.h"
#include "tls_transcript_hash.h"
#include "tls_ffdhe.h"
#include "tls_misc.h"
#include "tls13_misc.h"
#include "tls13_key_material.h"
#include "certificate/pem_import.h"
#include "kdf/hkdf.h"
#include "debug.h"

//Check TLS library configuration
#if (TLS_SUPPORT == ENABLED && TLS_MAX_VERSION >= TLS_VERSION_1_3)

//Downgrade protection mechanism (TLS 1.1 or below)
const uint8_t tls11DowngradeRandom[8] =
{
   0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x00
};

//Downgrade protection mechanism (TLS 1.2)
const uint8_t tls12DowngradeRandom[8] =
{
   0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01
};

//Special random value for HelloRetryRequest message
const uint8_t tls13HelloRetryRequestRandom[32] =
{
   0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
   0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
   0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
   0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
};


/**
 * @brief Compute PSK binder value
 * @param[in] context Pointer to the TLS context
 * @param[in] clientHello Pointer to the ClientHello message
 * @param[in] clientHelloLen Length of the ClientHello message
 * @param[in] truncatedClientHelloLen Length of the partial ClientHello message
 * @param[in] identity Pointer to the PSK identity
 * @param[out] binder Buffer where to store the resulting PSK binder
 * @param[in] binderLen Expected length of the PSK binder
 * @return Error code
 **/

error_t tls13ComputePskBinder(TlsContext *context, const void *clientHello,
   size_t clientHelloLen, size_t truncatedClientHelloLen,
   const Tls13PskIdentity *identity, uint8_t *binder, size_t binderLen)
{
   error_t error;
   const HashAlgo *hash;
   uint8_t *hashContext;
   uint8_t key[TLS_MAX_HKDF_DIGEST_SIZE];
   uint8_t digest[TLS_MAX_HKDF_DIGEST_SIZE];

   //Check parameters
   if(truncatedClientHelloLen >= clientHelloLen)
      return ERROR_INVALID_PARAMETER;

   //The hash function used by HKDF is the cipher suite hash algorithm
   hash = context->cipherSuite.prfHashAlgo;
   //Make sure the hash algorithm is valid
   if(hash == NULL)
      return ERROR_FAILURE;

   //Check the length of the PSK binder
   if(binderLen != hash->digestSize)
      return ERROR_INVALID_LENGTH;

   //Allocate a memory buffer to hold the hash context
   hashContext = tlsAllocMem(hash->contextSize);
   //Failed to allocate memory?
   if(hashContext == NULL)
      return ERROR_OUT_OF_MEMORY;

   //Intialize transcript hash
   if(context->handshakeHashContext != NULL)
      memcpy(hashContext, context->handshakeHashContext, hash->contextSize);
   else
      hash->init(hashContext);

#if (DTLS_SUPPORT == ENABLED)
   //DTLS protocol?
   if(context->transportProtocol == TLS_TRANSPORT_PROTOCOL_DATAGRAM)
   {
      DtlsHandshake header;

      //Handshake message type
      header.msgType = TLS_TYPE_CLIENT_HELLO;
      //Number of bytes in the message
      STORE24BE(clientHelloLen, header.length);
      //Message sequence number
      header.msgSeq = htons(context->txMsgSeq);
      //Fragment offset
      STORE24BE(0, header.fragOffset);
      //Fragment length
      STORE24BE(clientHelloLen, header.fragLength);

      //Digest the handshake message header
      hash->update(hashContext, &header, sizeof(DtlsHandshake));
   }
   else
#endif
   //TLS protocol?
   {
      TlsHandshake header;

      //Handshake message type
      header.msgType = TLS_TYPE_CLIENT_HELLO;
      //Number of bytes in the message
      STORE24BE(clientHelloLen, header.length);

      //Digest the handshake message header
      hash->update(hashContext, &header, sizeof(TlsHandshake));
   }

   //Digest the partial ClientHello
   hash->update(hashContext, clientHello, truncatedClientHelloLen);
   //Calculate transcript hash
   hash->final(hashContext, digest);

   //Release previously allocated memory
   tlsFreeMem(hashContext);

   //Debug message
   TRACE_DEBUG("Transcript hash (partial ClientHello):\r\n");
   TRACE_DEBUG_ARRAY("  ", digest, hash->digestSize);

   //Although PSKs can be established out of band, PSKs can also be established
   //in a previous connection
   if(tls13IsPskValid(context))
   {
      //Calculate early secret
      error = hkdfExtract(hash, context->psk, context->pskLen, NULL, 0,
         context->secret);
      //Any error to report?
      if(error)
         return error;

      //Debug message
      TRACE_DEBUG("Early secret:\r\n");
      TRACE_DEBUG_ARRAY("  ", context->secret, hash->digestSize);

      //Calculate binder key
      error = tls13DeriveSecret(context, context->secret, hash->digestSize,
         "ext binder", "", 0, key, hash->digestSize);
      //Any error to report?
      if(error)
         return error;
   }
   else if(tls13IsTicketValid(context))
   {
      //Calculate early secret
      error = hkdfExtract(hash, context->ticketPsk, context->ticketPskLen,
         NULL, 0, context->secret);
      //Any error to report?
      if(error)
         return error;

      //Debug message
      TRACE_DEBUG("Early secret:\r\n");
      TRACE_DEBUG_ARRAY("  ", context->secret, hash->digestSize);

      //Calculate binder key
      error = tls13DeriveSecret(context, context->secret, hash->digestSize,
         "res binder", "", 0, key, hash->digestSize);
      //Any error to report?
      if(error)
         return error;
   }
   else
   {
      //The pre-shared key is not valid
      return ERROR_FAILURE;
   }

   //Debug message
   TRACE_DEBUG("Binder key:\r\n");
   TRACE_DEBUG_ARRAY("  ", key, hash->digestSize);

   //The PskBinderEntry is computed in the same way as the Finished message
   //but with the base key being the binder key
   error = tls13HkdfExpandLabel(hash, key, hash->digestSize, "finished",
      NULL, 0, key, hash->digestSize);
   //Any error to report?
   if(error)
      return error;

   //Debug message
   TRACE_DEBUG("Finished key:\r\n");
   TRACE_DEBUG_ARRAY("  ", key, hash->digestSize);

   //Compute PSK binder
   error = hmacCompute(hash, key, hash->digestSize, digest, hash->digestSize,
      binder);
   //Any error to report?
   if(error)
      return error;

   //Debug message
   TRACE_DEBUG("PSK binder:\r\n");
   TRACE_DEBUG_ARRAY("  ", binder, binderLen);

   //Successful processing
   return NO_ERROR;
}


/**
 * @brief Key share generation
 * @param[in] context Pointer to the TLS context
 * @param[in] namedGroup Named group
 * @return Error code
 **/

error_t tls13GenerateKeyShare(TlsContext *context, uint16_t namedGroup)
{
   error_t error;

#if (TLS13_ECDHE_KE_SUPPORT == ENABLED || TLS13_PSK_ECDHE_KE_SUPPORT == ENABLED)
   //Elliptic curve group?
   if(tls13IsEcdheGroupSupported(context, namedGroup))
   {
      const EcCurveInfo *curveInfo;

      //Retrieve the elliptic curve to be used
      curveInfo = tlsGetCurveInfo(context, namedGroup);

      //Valid elliptic curve?
      if(curveInfo != NULL)
      {
         //Save the named group
         context->namedGroup = namedGroup;

         //Load EC domain parameters
         error = ecLoadDomainParameters(&context->ecdhContext.params, curveInfo);

         //Check status code
         if(!error)
         {
            //Generate an ephemeral key pair
            error = ecdhGenerateKeyPair(&context->ecdhContext, context->prngAlgo,
               context->prngContext);
         }
      }
      else
      {
         //Unsupported elliptic curve
         error = ERROR_ILLEGAL_PARAMETER;
      }
   }
   else
#endif
#if (TLS13_DHE_KE_SUPPORT == ENABLED || TLS13_PSK_DHE_KE_SUPPORT == ENABLED)
   //Finite field group?
   if(tls13IsFfdheGroupSupported(context, namedGroup))
   {
#if (TLS_FFDHE_SUPPORT == ENABLED)
      const TlsFfdheGroup *ffdheGroup;

      //Get the FFDHE parameters that match the specified named group
      ffdheGroup = tlsGetFfdheGroup(context, namedGroup);

      //Valid FFDHE group?
      if(ffdheGroup != NULL)
      {
         //Save the named group
         context->namedGroup = namedGroup;

         //Load FFDHE parameters
         error = tlsLoadFfdheParameters(&context->dhContext.params, ffdheGroup);

         //Check status code
         if(!error)
         {
            //Generate an ephemeral key pair
            error = dhGenerateKeyPair(&context->dhContext, context->prngAlgo,
               context->prngContext);
         }
      }
      else
#endif
      {
         //The specified FFDHE group is not supported
         error = ERROR_ILLEGAL_PARAMETER;
      }
   }
   else
#endif
   //Unknown group?
   {
      //Report an error
      error = ERROR_ILLEGAL_PARAMETER;
   }

   //Return status code
   return error;
}


/**
 * @brief (EC)DHE shared secret generation
 * @param[in] context Pointer to the TLS context
 * @param[in] keyShare Pointer to the peer's (EC)DHE parameters
 * @param[in] length Length of the (EC)DHE parameters, in bytes
 * @return Error code
 **/

error_t tls13GenerateSharedSecret(TlsContext *context, const uint8_t *keyShare,
   size_t length)
{
   error_t error;

#if (TLS13_ECDHE_KE_SUPPORT == ENABLED || TLS13_PSK_ECDHE_KE_SUPPORT == ENABLED)
   //Elliptic curve group?
   if(tls13IsEcdheGroupSupported(context, context->namedGroup))
   {
      //Read peer's public key (refer to RFC 8446, section 4.2.8.2)
      error = ecImport(&context->ecdhContext.params,
         &context->ecdhContext.qb, keyShare, length);

      //Check status code
      if(!error)
      {
         //Verify peer's public key
         error = ecdhCheckPublicKey(&context->ecdhContext.params,
            &context->ecdhContext.qb);
      }

      //Check status code
      if(!error)
      {
         //ECDH shared secret calculation is performed according to IEEE Std
         //1363-2000 (refer to RFC 8446, section 7.4.2)
         error = ecdhComputeSharedSecret(&context->ecdhContext,
            context->premasterSecret, TLS_PREMASTER_SECRET_SIZE,
            &context->premasterSecretLen);
      }
   }
   else
#endif
#if (TLS13_DHE_KE_SUPPORT == ENABLED || TLS13_PSK_DHE_KE_SUPPORT == ENABLED)
   //Finite field group?
   if(tls13IsFfdheGroupSupported(context, context->namedGroup))
   {
#if (TLS_FFDHE_SUPPORT == ENABLED)
      //Read client's public key (refer to RFC 8446, section 4.2.8.1)
      error = mpiImport(&context->dhContext.yb, keyShare, length,
         MPI_FORMAT_BIG_ENDIAN);

      //Check status code
      if(!error)
      {
         //Verify peer's public key
         error = dhCheckPublicKey(&context->dhContext.params,
            &context->dhContext.yb);
      }

      //Check status code
      if(!error)
      {
         //The negotiated key (Z) is converted to a byte string by encoding in
         //big-endian and left padded with zeros up to the size of the prime
         //(refer to RFC 8446, section 7.4.1)
         error = dhComputeSharedSecret(&context->dhContext,
            context->premasterSecret, TLS_PREMASTER_SECRET_SIZE,
            &context->premasterSecretLen);
      }
#else
      //The specified FFDHE group is not supported
      error = ERROR_HANDSHAKE_FAILED;
#endif
   }
   else
#endif
   //Unknown group?
   {
      //Report an error
      error = ERROR_HANDSHAKE_FAILED;
   }

   //Return status code
   return error;
}


/**
 * @brief Digital signature generation (TLS 1.3)
 * @param[in] context Pointer to the TLS context
 * @param[out] p Buffer where to store the digitally-signed element
 * @param[out] length Length of the digitally-signed element
 * @return Error code
 **/

error_t tls13GenerateSignature(TlsContext *context, uint8_t *p,
   size_t *length)
{
   error_t error;
   size_t n;
   uint8_t *buffer;
   Tls13DigitalSignature *signature;
   const HashAlgo *hashAlgo;

   //Point to the digitally-signed element
   signature = (Tls13DigitalSignature *) p;

   //The hash function used by HKDF is the cipher suite hash algorithm
   hashAlgo = context->cipherSuite.prfHashAlgo;
   //Make sure the hash algorithm is valid
   if(hashAlgo == NULL)
      return ERROR_FAILURE;

   //Calculate the length of the content covered by the digital signature
   n = hashAlgo->digestSize + 98;

   //Allocate a memory buffer
   buffer = tlsAllocMem(n);
   //Failed to allocate memory?
   if(buffer == NULL)
      return ERROR_OUT_OF_MEMORY;

   //Form a string that consists of octet 32 (0x20) repeated 64 times
   memset(buffer, ' ', 64);

   //Append the context string. It is used to provide separation between
   //signatures made in different contexts, helping against potential
   //cross-protocol attacks
   if(context->entity == TLS_CONNECTION_END_CLIENT)
      memcpy(buffer + 64, "TLS 1.3, client CertificateVerify", 33);
   else
      memcpy(buffer + 64, "TLS 1.3, server CertificateVerify", 33);

   //Append a single 0 byte which serves as the separator
   buffer[97] = 0x00;

   //Compute the transcript hash
   error = tlsFinalizeTranscriptHash(context, hashAlgo,
      context->handshakeHashContext, "", buffer + 98);

   //Check status code
   if(!error)
   {
#if (TLS_RSA_PSS_SIGN_SUPPORT == ENABLED)
      //RSA-PSS signature scheme?
      if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA256 ||
         context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA384 ||
         context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA512 ||
         context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA256 ||
         context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA384 ||
         context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA512)
      {
         RsaPrivateKey privateKey;

         //Initialize RSA private key
         rsaInitPrivateKey(&privateKey);

         //The algorithm field specifies the signature scheme and the
         //corresponding hash algorithm
         if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA256)
         {
            //Select rsa_pss_rsae_sha256 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA256);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA384)
         {
            //Select rsa_pss_rsae_sha384 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA384);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_RSAE_SHA512)
         {
            //Select rsa_pss_rsae_sha512 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA512);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA256)
         {
            //Select rsa_pss_pss_sha256 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA256);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA384)
         {
            //Select rsa_pss_pss_sha384 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA384);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_RSA_PSS_PSS_SHA512)
         {
            //Select rsa_pss_pss_sha512 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA512);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
         }
         else
         {
            //Invalid signature algorithm
            error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
         }

         //Check status code
         if(!error)
         {
            //Pre-hash the content covered by the digital signature
            if(hashAlgo != NULL)
               error = hashAlgo->compute(buffer, n, context->clientVerifyData);
            else
               error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
         }

         //Check status code
         if(!error)
         {
            //Retrieve the RSA private key corresponding to the certificate sent
            //in the previous message
            error = pemImportRsaPrivateKey(context->cert->privateKey,
               context->cert->privateKeyLen, &privateKey);
         }

         //Check status code
         if(!error)
         {
            //RSA signatures must use an RSASSA-PSS algorithm, regardless of
            //whether RSASSA-PKCS1-v1_5 algorithms appear in SignatureAlgorithms
            error = rsassaPssSign(context->prngAlgo, context->prngContext,
               &privateKey, hashAlgo, hashAlgo->digestSize,
               context->clientVerifyData, signature->value, length);
         }

         //Release previously allocated resources
         rsaFreePrivateKey(&privateKey);
      }
      else
#endif
#if (TLS_ECDSA_SIGN_SUPPORT == ENABLED)
      //ECDSA signature scheme?
      if(context->signAlgo == TLS_SIGN_ALGO_ECDSA)
      {
         //The algorithm field specifies the signature scheme, the corresponding
         //curve and the corresponding hash algorithm
         if(context->cert->namedCurve == TLS_GROUP_SECP256R1 &&
            context->signHashAlgo == TLS_HASH_ALGO_SHA256)
         {
            //Select ecdsa_secp256r1_sha256 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_ECDSA_SECP256R1_SHA256);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
         }
         else if(context->cert->namedCurve == TLS_GROUP_SECP384R1 &&
            context->signHashAlgo == TLS_HASH_ALGO_SHA384)
         {
            //Select ecdsa_secp384r1_sha384 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_ECDSA_SECP384R1_SHA384);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
         }
         else if(context->cert->namedCurve == TLS_GROUP_SECP521R1 &&
            context->signHashAlgo == TLS_HASH_ALGO_SHA512)
         {
            //Select ecdsa_secp521r1_sha512 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_ECDSA_SECP521R1_SHA512);
            hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
         }
         else
         {
            //Invalid signature algorithm
            error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
         }

         //Check status code
         if(!error)
         {
            //Pre-hash the content covered by the digital signature
            if(hashAlgo != NULL)
               error = hashAlgo->compute(buffer, n, context->clientVerifyData);
            else
               error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
         }

         //Check status code
         if(!error)
         {
            //Generate an ECDSA signature
            error = tlsGenerateEcdsaSignature(context, context->clientVerifyData,
               hashAlgo->digestSize, signature->value, length);
         }
      }
      else
#endif
#if (TLS_EDDSA_SIGN_SUPPORT == ENABLED)
      //EdDSA signature scheme?
      if(context->signAlgo == TLS_SIGN_ALGO_ED25519 ||
         context->signAlgo == TLS_SIGN_ALGO_ED448)
      {
         //The algorithm field specifies the signature algorithm used
         if(context->signAlgo == TLS_SIGN_ALGO_ED25519)
         {
            //Select ed25519 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_ED25519);
         }
         else if(context->signAlgo == TLS_SIGN_ALGO_ED448)
         {
            //Select ed448 signature algorithm
            signature->algorithm = HTONS(TLS_SIGN_SCHEME_ED448);
         }
         else
         {
            //Invalid signature algorithm
            error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
         }

         //Check status code
         if(!error)
         {
            //Generate a signature in PureEdDSA mode, without pre-hashing
            error = tlsGenerateEddsaSignature(context, buffer, n,
               signature->value, length);
         }
      }
      else
#endif
      //Invalid signature scheme?
      {
         //Report an error
         error = ERROR_UNSUPPORTED_SIGNATURE_ALGO;
      }
   }

   //Release memory buffer
   tlsFreeMem(buffer);

   //Check status code
   if(!error)
   {
      //The signature is preceded by a 2-byte length field
      signature->length = htons(*length);
      //Total length of the digitally-signed element
      *length += sizeof(Tls13DigitalSignature);
   }

   //Return status code
   return error;
}


/**
 * @brief Digital signature verification (TLS 1.3)
 * @param[in] context Pointer to the TLS context
 * @param[in] p Pointer to the digitally-signed element to be verified
 * @param[in] length Length of the digitally-signed element
 * @return Error code
 **/

error_t tls13VerifySignature(TlsContext *context, const uint8_t *p,
   size_t length)
{
   error_t error;
   size_t n;
   uint8_t *buffer;
   Tls13SignatureScheme signAlgo;
   const Tls13DigitalSignature *signature;
   const HashAlgo *hashAlgo;

   //Point to the digitally-signed element
   signature = (Tls13DigitalSignature *) p;

   //Malformed CertificateVerify message?
   if(length < sizeof(Tls13DigitalSignature))
      return ERROR_DECODING_FAILED;
   if(length != (sizeof(Tls13DigitalSignature) + ntohs(signature->length)))
      return ERROR_DECODING_FAILED;

   //The hash function used by HKDF is the cipher suite hash algorithm
   hashAlgo = context->cipherSuite.prfHashAlgo;
   //Make sure the hash algorithm is valid
   if(hashAlgo == NULL)
      return ERROR_FAILURE;

   //Calculate the length of the content covered by the digital signature
   n = hashAlgo->digestSize + 98;

   //Allocate a memory buffer
   buffer = tlsAllocMem(n);
   //Failed to allocate memory?
   if(buffer == NULL)
      return ERROR_OUT_OF_MEMORY;

   //Form a string that consists of octet 32 (0x20) repeated 64 times
   memset(buffer, ' ', 64);

   //Append the context string. It is used to provide separation between
   //signatures made in different contexts, helping against potential
   //cross-protocol attacks
   if(context->entity == TLS_CONNECTION_END_CLIENT)
      memcpy(buffer + 64, "TLS 1.3, server CertificateVerify", 33);
   else
      memcpy(buffer + 64, "TLS 1.3, client CertificateVerify", 33);

   //Append a single 0 byte which serves as the separator
   buffer[97] = 0x00;

   //Compute the transcript hash
   error = tlsFinalizeTranscriptHash(context, hashAlgo,
      context->handshakeHashContext, "", buffer + 98);

   //Check status code
   if(!error)
   {
      //The algorithm field specifies the signature scheme
      signAlgo = (Tls13SignatureScheme) ntohs(signature->algorithm);

#if (TLS_RSA_PSS_SIGN_SUPPORT == ENABLED)
      //RSASSA-PSS signature scheme?
      if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA256 ||
         signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA384 ||
         signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA512 ||
         signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA256 ||
         signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA384 ||
         signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA512)
      {
         //Enforce the type of the certificate provided by the peer
         if(context->peerCertType == TLS_CERT_RSA_SIGN)
         {
            //Retrieve the hash algorithm used for signing
            if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA256)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
            else if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA384)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
            else if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_RSAE_SHA512)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
            else
               hashAlgo = NULL;
         }
         else if(context->peerCertType == TLS_CERT_RSA_PSS_SIGN)
         {
            //Retrieve the hash algorithm used for signing
            if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA256)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
            else if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA384)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
            else if(signAlgo == TLS_SIGN_SCHEME_RSA_PSS_PSS_SHA512)
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
            else
               hashAlgo = NULL;
         }
         else
         {
            //Invalid certificate
            hashAlgo = NULL;
         }

         //Pre-hash the content covered by the digital signature
         if(hashAlgo != NULL)
            error = hashAlgo->compute(buffer, n, context->clientVerifyData);
         else
            error = ERROR_INVALID_SIGNATURE;

         //Check status code
         if(!error)
         {
            //Verify RSASSA-PSS signature
            error = rsassaPssVerify(&context->peerRsaPublicKey, hashAlgo,
               hashAlgo->digestSize, context->clientVerifyData,
               signature->value, ntohs(signature->length));
         }
      }
      else
#endif
#if (TLS_ECDSA_SIGN_SUPPORT == ENABLED)
      //ECDSA signature scheme?
      if(signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP256R1_SHA256 ||
         signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP384R1_SHA384 ||
         signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP521R1_SHA512)
      {
         //Enforce the type of the certificate provided by the peer
         if(context->peerCertType == TLS_CERT_ECDSA_SIGN)
         {
            //Retrieve the hash algorithm used for signing
            if(context->peerEcParams.name == NULL)
            {
               //Invalid signature scheme
               hashAlgo = NULL;
            }
            else if(signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP256R1_SHA256 &&
               strcmp(context->peerEcParams.name, "secp256r1") == 0)
            {
               //Select SHA-256 hash algorithm
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA256);
            }
            else if(signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP384R1_SHA384 &&
               strcmp(context->peerEcParams.name, "secp384r1") == 0)
            {
               //Select SHA-384 hash algorithm
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA384);
            }
            else if(signAlgo == TLS_SIGN_SCHEME_ECDSA_SECP521R1_SHA512 &&
               strcmp(context->peerEcParams.name, "secp521r1") == 0)
            {
               //Select SHA-512 hash algorithm
               hashAlgo = tlsGetHashAlgo(TLS_HASH_ALGO_SHA512);
            }
            else
            {
               //Invalid signature scheme
               hashAlgo = NULL;
            }
         }
         else
         {
            //Invalid certificate
            hashAlgo = NULL;
         }

         //Pre-hash the content covered by the digital signature
         if(hashAlgo != NULL)
            error = hashAlgo->compute(buffer, n, context->clientVerifyData);
         else
            error = ERROR_INVALID_SIGNATURE;

         //Check status code
         if(!error)
         {
            //Verify ECDSA signature
            error = tlsVerifyEcdsaSignature(context, context->clientVerifyData,
               hashAlgo->digestSize, signature->value, ntohs(signature->length));
         }
      }
      else
#endif
#if (TLS_EDDSA_SIGN_SUPPORT == ENABLED && TLS_ED25519_SUPPORT == ENABLED)
      //Ed25519 signature scheme?
      if(signAlgo == TLS_SIGN_SCHEME_ED25519)
      {
         //Enforce the type of the certificate provided by the peer
         if(context->peerCertType == TLS_CERT_ED25519_SIGN)
         {
            //Verify EdDSA signature (PureEdDSA mode)
            error = tlsVerifyEddsaSignature(context, buffer, n,
               signature->value, ntohs(signature->length));
         }
         else
         {
            //Invalid certificate
            error = ERROR_INVALID_SIGNATURE;
         }
      }
      else
#endif
#if (TLS_EDDSA_SIGN_SUPPORT == ENABLED && TLS_ED448_SUPPORT == ENABLED)
      //Ed448 signature scheme?
      if(signAlgo == TLS_SIGN_SCHEME_ED448)
      {
         //Enforce the type of the certificate provided by the peer
         if(context->peerCertType == TLS_CERT_ED448_SIGN)
         {
            //Verify EdDSA signature (PureEdDSA mode)
            error = tlsVerifyEddsaSignature(context, buffer, n,
               signature->value, ntohs(signature->length));
         }
         else
         {
            //Invalid certificate
            error = ERROR_INVALID_SIGNATURE;
         }
      }
      else
#endif
      //Unknown signature scheme?
      {
         //Report an error
         error = ERROR_INVALID_SIGNATURE;
      }
   }

   //Release memory buffer
   tlsFreeMem(buffer);

   //Return status code
   return error;
}


/**
 * @brief Hash ClientHello1 in the transcript when HelloRetryRequest is used
 * @param[in] context Pointer to the TLS context
 * @return Error code
 **/

error_t tls13DigestClientHello1(TlsContext *context)
{
   TlsHandshake *message;
   const HashAlgo *hash;

   //Invalid has context?
   if(context->handshakeHashContext == NULL)
      return ERROR_FAILURE;

   //The hash function used by HKDF is the cipher suite hash algorithm
   hash = context->cipherSuite.prfHashAlgo;
   //Make sure the hash algorithm is valid
   if(hash == NULL)
      return ERROR_FAILURE;

   //Point to the buffer where to format the handshake message
   message = (TlsHandshake *) context->txBuffer;

   //Handshake message type
   message->msgType = TLS_TYPE_MESSAGE_HASH;
   //Number of bytes in the message
   STORE24BE(hash->digestSize, message->length);

   //Compute Hash(ClientHello1)
   hash->final(context->handshakeHashContext, message->data);
   //Re-initialize hash algorithm context
   hash->init(context->handshakeHashContext);

   //When the server responds to a ClientHello with a HelloRetryRequest, the
   //value of ClientHello1 is replaced with a special synthetic handshake
   //message of handshake type MessageHash containing Hash(ClientHello1)
   hash->update(context->handshakeHashContext, message,
      hash->digestSize + sizeof(TlsHandshake));

   //Successful processing
   return NO_ERROR;
}


/**
 * @brief Check whether an externally established PSK is valid
 * @param[in] context Pointer to the TLS context
 * @return TRUE is the PSK is valid, else FALSE
 **/

bool_t tls13IsPskValid(TlsContext *context)
{
   bool_t valid = FALSE;

   //Make sure the hash algorithm associated with the PSK is valid
   if(tlsGetHashAlgo(context->pskHashAlgo) != NULL)
   {
      //Valid PSK?
      if(context->psk != NULL && context->pskLen > 0)
      {
         //Check whether TLS operates as a client or a server
         if(context->entity == TLS_CONNECTION_END_CLIENT)
         {
            //Valid PSK identity?
            if(context->pskIdentity != NULL)
            {
               valid = TRUE;
            }
         }
         else
         {
            valid = TRUE;
         }
      }
   }

   //Return TRUE is the PSK is valid, else FALSE
   return valid;
}


/**
 * @brief Check whether a session ticket is valid
 * @param[in] context Pointer to the TLS context
 * @return TRUE is the session ticket is valid, else FALSE
 **/

bool_t tls13IsTicketValid(TlsContext *context)
{
   bool_t valid = FALSE;

   //Make sure the hash algorithm associated with the ticket is valid
   if(tlsGetHashAlgo(context->ticketHashAlgo) != NULL)
   {
      //Valid ticket PSK?
      if(context->ticketPskLen > 0)
      {
         //Check whether TLS operates as a client or a server
         if(context->entity == TLS_CONNECTION_END_CLIENT)
         {
            //Valid ticket?
            if(context->ticket != NULL && context->ticketLen > 0)
            {
               valid = TRUE;
            }
         }
         else
         {
            valid = TRUE;
         }
      }
   }

   //Return TRUE is the ticket is valid, else FALSE
   return valid;
}


/**
 * @brief Check whether a given named group is supported
 * @param[in] context Pointer to the TLS context
 * @param[in] namedGroup Named group
 * @return TRUE is the named group is supported, else FALSE
 **/

bool_t tls13IsGroupSupported(TlsContext *context, uint16_t namedGroup)
{
   bool_t acceptable;

   //Initialize flag
   acceptable = FALSE;

   //Check whether the ECDHE of FFDHE group is supported
   if(tls13IsEcdheGroupSupported(context, namedGroup))
   {
      acceptable = TRUE;
   }
   else if(tls13IsFfdheGroupSupported(context, namedGroup))
   {
      acceptable = TRUE;
   }
   else
   {
      acceptable = FALSE;
   }

   //Return TRUE is the named group is supported
   return acceptable;
}


/**
 * @brief Check whether a given ECDHE group is supported
 * @param[in] context Pointer to the TLS context
 * @param[in] namedGroup Named group
 * @return TRUE is the ECDHE group is supported, else FALSE
 **/

bool_t tls13IsEcdheGroupSupported(TlsContext *context, uint16_t namedGroup)
{
   bool_t acceptable;

   //Initialize flag
   acceptable = FALSE;

#if (TLS13_ECDHE_KE_SUPPORT == ENABLED || TLS13_PSK_ECDHE_KE_SUPPORT == ENABLED)
   //Elliptic curve group?
   if(namedGroup == TLS_GROUP_SECP224R1 ||
      namedGroup == TLS_GROUP_SECP256R1 ||
      namedGroup == TLS_GROUP_SECP384R1 ||
      namedGroup == TLS_GROUP_SECP521R1 ||
      namedGroup == TLS_GROUP_ECDH_X25519 ||
      namedGroup == TLS_GROUP_ECDH_X448)
   {
      //Check whether the ECDHE group is supported
      if(tlsGetCurveInfo(context, namedGroup) != NULL)
      {
         acceptable = TRUE;
      }
   }
#endif

   //Return TRUE is the named group is supported
   return acceptable;
}


/**
 * @brief Check whether a given FFDHE group is supported
 * @param[in] context Pointer to the TLS context
 * @param[in] namedGroup Named group
 * @return TRUE is the FFDHE group is supported, else FALSE
 **/

bool_t tls13IsFfdheGroupSupported(TlsContext *context, uint16_t namedGroup)
{
   bool_t acceptable;

   //Initialize flag
   acceptable = FALSE;

#if (TLS13_DHE_KE_SUPPORT == ENABLED || TLS13_PSK_DHE_KE_SUPPORT == ENABLED)
   //Finite field group?
   if(namedGroup == TLS_GROUP_FFDHE2048 ||
      namedGroup == TLS_GROUP_FFDHE3072 ||
      namedGroup == TLS_GROUP_FFDHE4096 ||
      namedGroup == TLS_GROUP_FFDHE6144 ||
      namedGroup == TLS_GROUP_FFDHE8192)
   {
#if (TLS_FFDHE_SUPPORT == ENABLED)
      //Check whether the FFDHE group is supported
      if(tlsGetFfdheGroup(context, namedGroup) != NULL)
      {
         acceptable = TRUE;
      }
#endif
   }
#endif

   //Return TRUE is the named group is supported
   return acceptable;
}


/**
 * @brief Check whether the specified key share group is a duplicate
 * @param[in] namedGroup Named group
 * @param[in] p List of key share entries
 * @param[in] length Length of the list, in bytes
 * @return Error code
 **/

error_t tls13CheckDuplicateKeyShare(uint16_t namedGroup, const uint8_t *p,
   size_t length)
{
   size_t n;
   const Tls13KeyShareEntry *keyShareEntry;

   //Parse the list of key share entries offered by the peer
   while(length > 0)
   {
      //Malformed extension?
      if(length < sizeof(Tls13KeyShareEntry))
         return ERROR_DECODING_FAILED;

      //Point to the current key share entry
      keyShareEntry = (Tls13KeyShareEntry *) p;
      //Retrieve the length of the key_exchange field
      n = ntohs(keyShareEntry->length);

      //Malformed extension?
      if(length < (sizeof(Tls13KeyShareEntry) + n))
         return ERROR_DECODING_FAILED;

      //Clients must not offer multiple KeyShareEntry values for the same
      //group. Servers may check for violations of this rule and abort the
      //handshake with an illegal_parameter alert
      if(ntohs(keyShareEntry->group) == namedGroup)
         return ERROR_ILLEGAL_PARAMETER;

      //Jump to the next key share entry
      p += sizeof(Tls13KeyShareEntry) + n;
      //Number of bytes left to process
      length -= sizeof(Tls13KeyShareEntry) + n;
   }

   //Successful verification
   return NO_ERROR;
}


/**
 * @brief Format certificate extensions
 * @param[in] p Output stream where to write the list of extensions
 * @param[out] written Total number of bytes that have been written
 * @return Error code
 **/

error_t tls13FormatCertExtensions(uint8_t *p, size_t *written)
{
   TlsExtensionList *extensionList;

   //Point to the list of extensions
   extensionList = (TlsExtensionList *) p;

   //Extensions in the Certificate message from the server must correspond to
   //ones from the ClientHello message. Extensions in the Certificate message
   //from the client must correspond to extensions in the CertificateRequest
   //message from the server
   extensionList->length = HTONS(0);

   //Total number of bytes that have been written
   *written = sizeof(TlsExtensionList);

   //Successful processing
   return NO_ERROR;
}


/**
 * @brief Parse certificate extensions
 * @param[in] p Input stream where to read the list of extensions
 * @param[in] length Number of bytes available in the input stream
 * @return Error code
 **/

error_t tls13ParseCertExtensions(const uint8_t *p, size_t length,
   size_t *consumed)
{
   error_t error;
   size_t n;
   TlsHelloExtensions extensions;
   const TlsExtensionList *extensionList;

   //Point to the list of extensions
   extensionList = (TlsExtensionList *) p;

   //Malformed CertificateEntry?
   if(length < sizeof(TlsExtensionList))
      return ERROR_DECODING_FAILED;

   //Retrieve the length of the list
   n = sizeof(TlsExtensionList) + ntohs(extensionList->length);

   //Malformed CertificateEntry?
   if(length < n)
      return ERROR_DECODING_FAILED;

   //Parse the list of extensions for the current CertificateEntry
   error = tlsParseHelloExtensions(TLS_TYPE_CERTIFICATE, p, n,
      &extensions);
   //Any error to report?
   if(error)
      return error;

   //Check the list of extensions
   error = tlsCheckHelloExtensions(TLS_TYPE_CERTIFICATE, TLS_VERSION_1_3,
      &extensions);
   //Any error to report?
   if(error)
      return error;

   //Total number of bytes that have been consumed
   *consumed = n;

   //Successful processing
   return NO_ERROR;
}

#endif