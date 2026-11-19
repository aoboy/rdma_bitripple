/**
 * @file bitripple_codec.c
 * @brief Encoder/Decoder implementation using nanorq library
 *
 * Implements FEC encoding and decoding using the nanorq
 * (Network-Friendly Quotient) library for RaptorQ/LT codes.
 */

#include <stdlib.h>
#include <string.h>
#include "bitripple.h"
#include "nanorq.h"
#include "io.h"

/*======================================================================*
 * Internal Structures                                                  *
 *======================================================================*/

/**
 * @brief Encoder internal state
 *
 * Wraps the nanorq encoder with additional metadata.
 */
struct br_encoder {
    nanorq* rq;              /**< nanorq encoder context */
    struct ioctx* io;           /**< Input/output context */
    uint16_t symbol_size;        /**< Symbol size in bytes */
    uint32_t num_blocks;        /**< Number of encoding blocks */
    uint64_t oti_common;      /**< Common OTI from encoder */
    uint32_t oti_scheme;       /**< Scheme-specific OTI */
};

/**
 * @brief Decoder internal state
 *
 * Wraps the nanorq decoder with output buffer management.
 */
struct br_decoder {
    nanorq* rq;              /**< nanorq decoder context */
    struct ioctx* io;           /**< Input/output context */
    uint8_t* out_buf;          /**< Decoded data buffer */
    size_t data_len;            /**< Actual data length */
    uint16_t symbol_size;       /**< Symbol size in bytes */
    uint32_t num_blocks;      /**< Number of blocks */
    int* repaired;            /**< Block repair status */
};

/*======================================================================*
 * Encoder Implementation                                              *
 *======================================================================*/

/**
 * @brief Create a new encoder
 *
 * Initializes the nanorq encoder with the input data.
 * Pre-generates symbols for all blocks to enable encoding.
 *
 * @param data Input data to encode
 * @param len Length of data in bytes
 * @param symbol_size Symbol size in bytes
 * @return New encoder, or NULL on error
 */
br_encoder_t* br_encoder_new(const void* data, size_t len, uint16_t symbol_size)
{
    if (!data || len == 0 || symbol_size == 0) {
        return NULL;
    }

    br_encoder_t* e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }

    e->symbol_size = symbol_size;
    e->io = ioctx_from_mem((const uint8_t*)data, len);
    if (!e->io) {
        free(e);
        return NULL;
    }

    /* Use Al = 8 to match upstream encode.c default */
    e->rq = nanorq_encoder_new(len, symbol_size, 8);
    if (!e->rq) {
        e->io->destroy(e->io);
        free(e);
        return NULL;
    }

    e->num_blocks = (uint32_t)nanorq_blocks(e->rq);

    /* Pre-generate symbols for all blocks */
    for (uint8_t sbn = 0; sbn < e->num_blocks; sbn++) {
        if (!nanorq_generate_symbols(e->rq, sbn, e->io)) {
            nanorq_free(e->rq);
            e->io->destroy(e->io);
            free(e);
            return NULL;
        }
    }

    e->oti_common = nanorq_oti_common(e->rq);
    e->oti_scheme = nanorq_oti_scheme_specific(e->rq);

    return e;
}

/**
 * @brief Free encoder
 * @param e Encoder to free
 */
void br_encoder_free(br_encoder_t* e)
{
    if (!e) {
        return;
    }

    if (e->rq) {
        nanorq_free(e->rq);
    }
    if (e->io) {
        e->io->destroy(e->io);
    }
    free(e);
}

/**
 * @brief Get number of encoding blocks
 * @param e Encoder
 * @return Number of blocks
 */
uint32_t br_encoder_num_blocks(br_encoder_t* e)
{
    return e ? e->num_blocks : 0;
}

/**
 * @brief Get number of source symbols in a block
 * @param e Encoder
 * @param sbn Block number
 * @return Number of source symbols
 */
uint32_t br_encoder_block_symbols(br_encoder_t* e, uint8_t sbn)
{
    if (!e || !e->rq) {
        return 0;
    }
    return (uint32_t)nanorq_block_symbols(e->rq, sbn);
}

/**
 * @brief Encode a symbol
 *
 * Generates a coded symbol for the specified block and ESI.
 *
 * @param e Encoder
 * @param sbn Block number
 * @param esi Encoding symbol index
 * @param buf Output buffer
 * @param out_len Output length
 * @return BR_OK on success
 */
int br_encoder_symbol(br_encoder_t* e, uint8_t sbn, uint32_t esi,
                  void* buf, size_t* out_len)
{
    if (!e || !buf || !out_len) {
        return BR_ERR_INVAL;
    }

    size_t written = nanorq_encode(e->rq, buf, esi, sbn, e->io);
    if (written == 0) {
        return BR_ERR_CODEC;
    }

    *out_len = written;
    return BR_OK;
}

/**
 * @brief Get object transfer information
 *
 * Returns the OTI values needed to initialize a matching decoder.
 *
 * @param e Encoder
 * @param common Common OTI output
 * @param scheme Scheme-specific OTI output
 */
void br_encoder_oti(br_encoder_t* e, uint64_t* common, uint32_t* scheme)
{
    if (!e) {
        return;
    }
    if (common) {
        *common = e->oti_common;
    }
    if (scheme) {
        *scheme = e->oti_scheme;
    }
}

/*======================================================================*
 * Decoder Implementation                                              *
 *======================================================================*/

/**
 * @brief Create a new decoder
 *
 * Initializes the nanorq decoder with OTI from the encoder.
 *
 * @param common Common OTI from encoder
 * @param scheme Scheme-specific OTI from encoder
 * @param data_len Expected data length (0 to use OTI)
 * @return New decoder, or NULL on error
 */
br_decoder_t* br_decoder_new(uint64_t common, uint32_t scheme, size_t data_len)
{
    if (data_len == 0) {
        return NULL;
    }

    br_decoder_t* d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->rq = nanorq_decoder_new(common, scheme);
    if (!d->rq) {
        free(d);
        return NULL;
    }

    /* Use length from OTI (may include padding) */
    size_t actual_len = nanorq_transfer_length(d->rq);
    d->data_len = actual_len;

    d->out_buf = calloc(1, actual_len);
    if (!d->out_buf) {
        nanorq_free(d->rq);
        free(d);
        return NULL;
    }

    d->io = ioctx_from_mem(d->out_buf, actual_len);
    if (!d->io) {
        free(d->out_buf);
        nanorq_free(d->rq);
        free(d);
        return NULL;
    }

    d->symbol_size = (uint16_t)nanorq_symbol_size(d->rq);
    d->num_blocks = (uint32_t)nanorq_blocks(d->rq);

    d->repaired = calloc(d->num_blocks, sizeof(int));
    if (!d->repaired) {
        d->io->destroy(d->io);
        free(d->out_buf);
        nanorq_free(d->rq);
        free(d);
        return NULL;
    }

    return d;
}

/**
 * @brief Free decoder
 * @param d Decoder to free
 */
void br_decoder_free(br_decoder_t* d)
{
    if (!d) {
        return;
    }

    free(d->repaired);
    if (d->rq) {
        nanorq_free(d->rq);
    }
    if (d->io) {
        d->io->destroy(d->io);
    }
    free(d->out_buf);
    free(d);
}

/**
 * @brief Feed a received symbol to decoder
 *
 * Adds a received symbol to the decoder context.
 *
 * @param d Decoder
 * @param sbn Block number
 * @param esi Encoding symbol index
 * @param buf Symbol data
 * @param len Symbol length
 * @return BR_OK on success
 */
int br_decoder_feed(br_decoder_t* d, uint8_t sbn, uint32_t esi,
                 const void* buf, size_t len)
{
    if (!d || !buf) {
        return BR_ERR_INVAL;
    }
    if (len != d->symbol_size) {
        return BR_ERR_INVAL;
    }

    uint32_t tag = nanorq_tag(sbn, esi);
    int rc = nanorq_decoder_add_symbol(d->rq, (void*)buf, tag, d->io);
    if (rc == NANORQ_SYM_ERR) {
        return BR_ERR_CODEC;
    }

    return BR_OK;
}

/**
 * @brief Check if decoder can reconstruct
 *
 * Attempts to repair each block. If repair succeeds,
 * the block is marked as repaired.
 *
 * @param d Decoder
 * @return Non-zero if all blocks repaired
 */
int br_decoder_ready(br_decoder_t* d)
{
    if (!d || !d->rq) {
        return 0;
    }

    for (uint8_t sbn = 0; sbn < d->num_blocks; sbn++) {
        if (!d->repaired[sbn]) {
            if (nanorq_repair_block(d->rq, d->io, sbn)) {
                d->repaired[sbn] = 1;
            } else {
                return 0;
            }
        }
    }

    return 1;
}

/**
 * @brief Reconstruct original data
 *
 * Reconstructs the original data from all repaired blocks.
 *
 * @param d Decoder
 * @param out Output buffer
 * @param len Buffer length
 * @return BR_OK on success
 */
int br_decoder_reconstruct(br_decoder_t* d, void* out, size_t len)
{
    if (!d || !out) {
        return BR_ERR_INVAL;
    }
    if (len < d->data_len) {
        return BR_ERR_INVAL;
    }

    /* Ensure all blocks are repaired */
    if (!br_decoder_ready(d)) {
        return BR_ERR_CODEC;
    }

    memcpy(out, d->out_buf, d->data_len);

    return BR_OK;
}