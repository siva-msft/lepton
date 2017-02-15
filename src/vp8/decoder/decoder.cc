#include "bool_decoder.hh"
#include "boolreader.hh"
#include "model.hh"
#include "../../lepton/idct.hh"
#include "decoder.hh"
using namespace std;


uint8_t prefix_unremap(uint8_t v) {
    if (v == 0) {
        return 0;
    }
    return v - 3;
}
#define LOG_DELTA_X_EDGE LogTable256[raster_to_aligned.kat<2>() - raster_to_aligned.kat<1>()]
#define LOG_DELTA_Y_EDGE LogTable256[raster_to_aligned.kat<16>() - raster_to_aligned.kat<8>()]
#ifdef _WIN32
#define log_delta_x_edge LOG_DELTA_X_EDGE
#define log_delta_y_edge LOG_DELTA_Y_EDGE

#else
enum {
    log_delta_x_edge = LOG_DELTA_X_EDGE,
    log_delta_y_edge = LOG_DELTA_Y_EDGE,
};
#endif

template<bool all_neighbors_present, BlockType color,
         bool horizontal>
void decode_one_edge(DecodeChannelContext chan_context,
                 BoolDecoder& decoder,
                 ProbabilityTables<all_neighbors_present, color> & probability_tables,
                 UniversalPrior &uprior,
                 uint8_t num_nonzeros_7x7, uint8_t est_eob,
                 ProbabilityTablesBase& pt) {

    ConstBlockContext context = chan_context.at(0).copy();

    uint8_t aligned_block_offset = raster_to_aligned.at(1);
    unsigned int log_edge_step = log_delta_x_edge;
    uint8_t delta = 1;
    uint8_t zig15offset = 0;
    if (!horizontal) {
        delta = 8;
        log_edge_step = log_delta_y_edge;
        zig15offset = 7;
        aligned_block_offset = raster_to_aligned.at(8);
    }
    uint8_t num_nonzeros_edge = 0;
    int16_t decoded_so_far = 0;
    for (int i= 2; i >=0; --i) {
        uprior.set_8x1_nz_bit_id(horizontal, i, decoded_so_far);
        Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
        int cur_bit = decoder.get(ubranch,
                                  Billing::NZ_EDGE) ? 1 : 0;
        probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
        num_nonzeros_edge |= (cur_bit << i);
        decoded_so_far <<= 1;
        decoded_so_far |= cur_bit;
    }
    if (num_nonzeros_edge > 7) {
        custom_exit(ExitCode::STREAM_INCONSISTENT);
    }
    uprior.set_nonzero_edge(horizontal, num_nonzeros_edge);
    unsigned int coord = delta;
    for (int lane = 0; lane < 7 && num_nonzeros_edge; ++lane, coord += delta, ++zig15offset) {
        ProbabilityTablesBase::CoefficientContext prior = {0, 0, 0};
        if (ProbabilityTablesBase::MICROVECTORIZE) {
            if (horizontal) {
                prior = probability_tables.update_coefficient_context8_horiz(coord,
                                                                             context,
                                                                             num_nonzeros_edge);
            } else {
                prior = probability_tables.update_coefficient_context8_vert(coord,
                                                                            context,
                                                                            num_nonzeros_edge);
            }
        } else {
	    prior = probability_tables.update_coefficient_context8(coord, context, num_nonzeros_edge);
        }
        uprior.update_by_prior(aligned_block_offset + (lane << log_edge_step), prior);
        uint8_t length = 0;
        bool nonzero = false;
        for (; length != MAX_EXPONENT; ++length) {
            uprior.set_8x1_exp_id(horizontal, length);
            Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
            int cur_bit = decoder.get(ubranch,
                                       (Billing)((int)Billing::BITMAP_EDGE + std::min((int)length, 4)));
            probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
            if (!cur_bit) {
                break;
            }
            nonzero = true;
        }
        int16_t coef = 0;
        if (nonzero) {
            uprior.update_nonzero_edge(horizontal, lane);
            uint8_t min_threshold = probability_tables.get_noise_threshold(coord);
            uprior.set_8x1_sign(horizontal);
            Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
            bool neg = !decoder.get(ubranch,
                                    Billing::SIGN_EDGE);
            probability_tables.update_universal_prob(pt, uprior, ubranch, neg ? 0 : 1);
            coef = (1 << (length - 1));
            --num_nonzeros_edge;

            if (length > 1){
                int i = length - 2;
                if (i >= min_threshold) {
                    for (; i >= min_threshold; --i) {
                        uprior.set_8x1_residual(horizontal, i, coef);
                        Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                        int cur_bit = (decoder.get(ubranch,
                                                   Billing::RES_EDGE) ? 1 : 0);
                        probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
                        coef |= (cur_bit << i);
                        // since we are not strict about rejecting jpegs with out of range coefs
                        // we just make those less efficient by reusing the same probability bucket
                    }
#ifdef ANNOTATION_ENABLED
                    probability_tables.residual_thresh_array_annot_update(coord, decoded_so_far >> 2);
#endif
                }
                for (; i >= 0; --i) {
                    uprior.set_8x1_residual(horizontal, i, coef);
                    Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                    int cur_bit = decoder.get(ubranch,
                                              Billing::RES_EDGE) ? 1 : 0;
                    coef |= (cur_bit << i);
                    probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
                }
            }
            if (neg) {
                coef = -coef;
            }
        }
        chan_context.at(0).here().raw_data()[aligned_block_offset + (lane << log_edge_step)] = coef;
        uprior.update_coef(aligned_block_offset + (lane << log_edge_step), coef);
    }
}

template<bool all_neighbors_present, BlockType color>
void decode_edge(DecodeChannelContext mcontext,
                 BoolDecoder& decoder,
                 ProbabilityTables<all_neighbors_present, color> & probability_tables,
                 UniversalPrior &uprior,
                 uint8_t num_nonzeros_7x7, uint8_t eob_x, uint8_t eob_y,
                 ProbabilityTablesBase& pt) {
    decode_one_edge<all_neighbors_present, color, true>(mcontext,
                                                                        decoder,
                                                                        probability_tables,
                                                        uprior,
                                                                        num_nonzeros_7x7,
                                                                        eob_x,
                                                                        pt);
    decode_one_edge<all_neighbors_present, color, false>(mcontext,
                                                                        decoder,
                                                                        probability_tables,
                                                         uprior,
                                                                        num_nonzeros_7x7,
                                                                        eob_y,
                                                                        pt);
}





template<bool all_neighbors_present, BlockType color>
void parse_tokens(DecodeChannelContext chan_context,
                  BoolDecoder& decoder,
                  ProbabilityTables<all_neighbors_present, color> & probability_tables,
                  ProbabilityTablesBase &pt) {
    UniversalPrior uprior;
    uprior.init(chan_context, color,
                all_neighbors_present || probability_tables.left_present,
                all_neighbors_present || probability_tables.above_present,
                all_neighbors_present || probability_tables.above_right_present);

    BlockContext context = chan_context.at(0);
    //BlockContext shadow_context0 = chan_context.at(1);
    //BlockContext shadow_context1 = chan_context.at(2);
    context.here().bzero();
    uint8_t num_nonzeros_7x7 = 0;
    int decoded_so_far = 0;
    for (int index = 5; index >= 0; --index) {
        uprior.set_7x7_nz_bit_id(index, decoded_so_far);
        Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
        int cur_bit = (decoder.get(ubranch,
                                   Billing::NZ_7x7)?1:0);
        probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
        num_nonzeros_7x7 |= (cur_bit << index);
        decoded_so_far <<= 1;
        decoded_so_far |= cur_bit;
    }
    if (num_nonzeros_7x7 > 49) {
        custom_exit(ExitCode::STREAM_INCONSISTENT); // this is a corrupt file: dont decode further
    }
    uprior.set_nonzeros7x7(num_nonzeros_7x7);
    uint8_t eob_x = 0;
    uint8_t eob_y = 0;
    uint8_t num_nonzeros_left_7x7 = num_nonzeros_7x7;
    Sirikata::AlignedArray1d<short, 8> avg;
    for (unsigned int zz = 0; zz < 49 && num_nonzeros_left_7x7; ++zz) {
        unsigned int coord = unzigzag49[zz];
        if ((zz & 7) == 0) {
#ifdef OPTIMIZED_7x7
            probability_tables.compute_aavrg_vec(zz, context.copy(), avg.begin());
#endif
        }
        unsigned int b_x = (coord & 7);
        unsigned int b_y = (coord >> 3);
        dev_assert((coord & 7) > 0 && (coord >> 3) > 0 && "this does the DC and the lower 7x7 AC");
        {
            ProbabilityTablesBase::CoefficientContext prior;

#ifdef OPTIMIZED_7x7
            prior = probability_tables.update_coefficient_context7x7_precomp(zz, avg[zz & 7], context.copy(), num_nonzeros_left_7x7);
#else
            prior = probability_tables.update_coefficient_context7x7(coord, zz, context.copy(), num_nonzeros_left_7x7);
#endif
            uprior.update_by_prior(zz + AlignedBlock::AC_7x7_INDEX, prior);
            uint8_t length;
            bool nonzero = false;
            for (length = 0; length != MAX_EXPONENT; ++length) {
                uprior.set_7x7_exp_id(length);
                Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                bool cur_bit = decoder.get(ubranch,
                                           (Billing)((unsigned int)Billing::BITMAP_7x7 +
                                                     std::min((int)length, 4)));
                probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
                if (!cur_bit) {
                    break;
                }
                nonzero = true;
            }
            int16_t coef = 0;
            bool neg = false;
            if (nonzero) {
                uprior.update_nonzero(b_x, b_y);
                --num_nonzeros_left_7x7;
                uprior.set_7x7_sign();
                Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                neg = !decoder.get(ubranch,
                                   Billing::SIGN_7x7);
                probability_tables.update_universal_prob(pt, uprior, ubranch, neg ? 0 : 1);
                eob_x = std::max(eob_x, (uint8_t)b_x);
                eob_y = std::max(eob_y, (uint8_t)b_y);
                coef = (1 << (length - 1));
                if (length > 1){
                    for (int i = length - 2; i >= 0; --i) {
                        uprior.set_7x7_residual(i, coef);
                        Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                        int cur_bit = decoder.get(ubranch,
                                                  Billing::RES_7x7);
                        coef |= ((cur_bit ? 1 : 0) << i);
                        probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
                    }
                }
                if (neg) {
                    coef = -coef;
                }
            }
#ifdef OPTIMIZED_7x7
            context.here().coef.at(zz + AlignedBlock::AC_7x7_INDEX) = coef;
            uprior.update_coef(zz + AlignedBlock::AC_7x7_INDEX, coef);
#else
            // this should work in all cases but doesn't utilize that the zz is related
            context.here().mutable_coefficients_raster(raster_to_aligned.at(coord)) = coef;
            uprior.update_coef(raster_to_aligned.at(coord), coef);
#endif
        }
    }
    decode_edge(chan_context,
                decoder,
                probability_tables,
                uprior,
                num_nonzeros_7x7, eob_x, eob_y,
                pt);
    Sirikata::AlignedArray1d<int16_t, 64> outp_sans_dc;
    int uncertainty = 0;
    int uncertainty2 = 0;
    int predicted_dc;
    if (advanced_dc_prediction) {
        predicted_dc = probability_tables.adv_predict_dc_pix(context.copy(), outp_sans_dc.begin(),
                                                             &uncertainty, &uncertainty2);
    } else {
        predicted_dc = probability_tables.predict_dc_dct(context.copy());
    }
    { // dc
        uint8_t length;
        bool nonzero = false;

        if (!advanced_dc_prediction) {
            ProbabilityTablesBase::CoefficientContext prior;

            prior = probability_tables.update_coefficient_context7x7(0, raster_to_aligned.at(0), context.copy(), num_nonzeros_7x7);
            uprior.update_by_prior(AlignedBlock::DC_INDEX, prior);
        } else {
            uprior.priors[UniversalPrior::OFFSET_BEST_PRIOR] = uncertainty;
            uprior.priors[UniversalPrior::OFFSET_BEST_PRIOR_SCALED] = uint16bit_length(abs(uncertainty));
            uprior.priors[UniversalPrior::OFFSET_BEST_PRIOR2] = uncertainty2;
            uprior.priors[UniversalPrior::OFFSET_BEST_PRIOR2_SCALED] = uint16bit_length(abs(uncertainty2));
            uprior.priors[UniversalPrior::OFFSET_ZZ_INDEX] = AlignedBlock::DC_INDEX;
        }
        for (length = 0; length < MAX_EXPONENT; ++length) {
            uprior.set_dc_exp_id(length);
            Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
            bool cur_bit = decoder.get(ubranch,
                                       (Billing)((int)Billing::EXP0_DC + std::min((int)length, 4)));
            probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit ? 1 : 0);
            if (!cur_bit) {
                break;
            }
            nonzero = true;
        }
        int16_t coef = 0;
        if (nonzero) {
            uprior.set_dc_sign();
            Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
            bool neg = !decoder.get(ubranch,Billing::SIGN_DC);
            probability_tables.update_universal_prob(pt, uprior, ubranch, neg ? 0 : 1);
            coef = (1 << (length - 1));
            if (length > 1){
                for (int i = length - 2; i >= 0; --i) {
                    uprior.set_dc_residual(i, coef);
                    Branch & ubranch=probability_tables.get_universal_prob(pt, uprior);
                    int cur_bit = decoder.get(ubranch,
                                              Billing::RES_DC);
                    coef |= ((cur_bit ? 1 : 0) << i);
                    probability_tables.update_universal_prob(pt, uprior, ubranch, cur_bit);
                }
            }
            if (neg) {
                coef = -coef;
            }
        }
        context.here().dc() = coef;
    }
    context.here().dc() = probability_tables.adv_predict_or_unpredict_dc(context.here().dc(),
                                                                         true,
                                                                         predicted_dc);
    context.num_nonzeros_here->set_num_nonzeros(num_nonzeros_7x7);

    context.num_nonzeros_here->set_horizontal(outp_sans_dc.begin(),
                                              ProbabilityTablesBase::quantization_table((int)color),
                                              context.here().dc());
    context.num_nonzeros_here->set_vertical(outp_sans_dc.begin(),
                                            ProbabilityTablesBase::quantization_table((int)color),
                                            context.here().dc());
}
#ifdef ALLOW_FOUR_COLORS
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<false, BlockType::Ck>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<true, BlockType::Ck>&, ProbabilityTablesBase&);
#endif

template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<false, BlockType::Y>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<false, BlockType::Cb>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<false, BlockType::Cr>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<true, BlockType::Y>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<true, BlockType::Cb>&, ProbabilityTablesBase&);
template void parse_tokens(DecodeChannelContext, BoolDecoder&, ProbabilityTables<true, BlockType::Cr>&, ProbabilityTablesBase&);
