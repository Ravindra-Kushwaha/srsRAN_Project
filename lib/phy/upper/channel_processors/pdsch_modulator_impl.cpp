/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "pdsch_modulator_impl.h"
#include "srsran/srsvec/bit.h"
#include "srsran/srsvec/sc_prod.h"
#include "srsran/support/math_utils.h"

using namespace srsran;

const bit_buffer& pdsch_modulator_impl::scramble(const bit_buffer& b, unsigned q, const config_t& config)
{
  temp_b_hat.resize(b.size());

  // Calculate initial scrambling state.
  unsigned c_init = (static_cast<unsigned>(config.rnti) << 15U) + (q << 14U) + config.n_id;

  // Initialize scrambling sequence.
  scrambler->init(c_init);

  // Apply scrambling sequence.
  scrambler->apply_xor(temp_b_hat, b);

  return temp_b_hat;
}

void pdsch_modulator_impl::modulate(span<cf_t>        d_pdsch,
                                    const bit_buffer& b_hat,
                                    modulation_scheme modulation,
                                    float             scaling)
{
  // Actual modulate.
  modulator->modulate(d_pdsch, b_hat, modulation);

  // Apply scaling only if the value is valid.
  if (std::isnormal(scaling)) {
    srsvec::sc_prod(d_pdsch, scaling, d_pdsch);
  }
}

void pdsch_modulator_impl::map(resource_grid_mapper& mapper, const re_buffer_reader& data_re, const config_t& config)
{
  // Get the PRB allocation mask.
  const bounded_bitset<MAX_RB> prb_allocation_mask =
      config.freq_allocation.get_prb_mask(config.bwp_start_rb, config.bwp_size_rb);

  // First symbol used in this transmission.
  unsigned start_symbol_index = config.start_symbol_index;

  // Calculate the end symbol index (excluded) and assert it does not exceed the slot boundary.
  unsigned end_symbol_index = config.start_symbol_index + config.nof_symbols;

  srsran_assert(end_symbol_index <= MAX_NSYMB_PER_SLOT,
                "The time allocation of the transmission ({}:{}) exceeds the slot boundary.",
                start_symbol_index,
                end_symbol_index);

  // PDSCH OFDM symbol mask.
  symbol_slot_mask symbols;
  symbols.fill(start_symbol_index, end_symbol_index);

  // Allocation pattern for the mapper.
  re_pattern_list allocation;
  re_pattern      pdsch_pattern;

  // Reserved REs, including DM-RS and CSI-RS.
  re_pattern_list reserved(config.reserved);

  // Get DM-RS RE pattern.
  re_pattern dmrs_pattern = config.dmrs_config_type.get_dmrs_pattern(
      config.bwp_start_rb, config.bwp_size_rb, config.nof_cdm_groups_without_data, config.dmrs_symb_pos);

  // Merge DM-RS RE pattern into the reserved RE patterns.
  reserved.merge(dmrs_pattern);

  // Set PDSCH allocation pattern.
  pdsch_pattern.prb_mask = prb_allocation_mask;
  pdsch_pattern.re_mask  = ~re_prb_mask();
  pdsch_pattern.symbols  = symbols;
  allocation.merge(pdsch_pattern);

  // Map into the resource grid.
  mapper.map(data_re, allocation, config.precoding, reserved);
}

void pdsch_modulator_impl::modulate(resource_grid_mapper&            mapper,
                                    span<const bit_buffer>           codewords,
                                    const pdsch_modulator::config_t& config)
{
  // Number of layers.
  unsigned nof_layers = config.precoding.get_nof_layers();
  srsran_assert(nof_layers > 0, "Number of layers cannot be zero.");
  srsran_assert(nof_layers <= 4, "More than four layers is not supported.");
  srsran_assert(codewords.size() == 1, "Only one PDSCH codeword is currently supported");

  modulation_scheme mod = config.modulation1;
  unsigned          Qm  = get_bits_per_symbol(mod);

  // Calculate number of REs.
  unsigned nof_bits = codewords[0].size();
  unsigned nof_re   = nof_bits / Qm;

  // Number of RE per layer, as per TS38.211 Section 7.3.1.3-1.
  unsigned nof_re_layer = nof_re / nof_layers;
  srsran_assert((nof_re % nof_layers == 0), "The number of modulated symbols cannot be equally split between layers.");

  // Resize the RE buffer.
  if ((nof_layers != temp_re.get_nof_slices()) || (nof_re_layer != temp_re.get_nof_re())) {
    temp_re.resize(nof_layers, nof_re_layer);
  }

  // Scramble.
  const bit_buffer& b_hat = scramble(codewords[0], 0, config);

  // View over the temporal PDSCH symbols buffer.
  span<cf_t> pdsch_symbols(temp_pdsch_symbols.begin(), nof_re);

  // Modulate codeword.
  modulate(pdsch_symbols, b_hat, mod, config.scaling);

  // Apply TS 38.211 Table 7.3.1.3-1: Codeword-to-layer mapping for spatial multiplexing.
  for (unsigned i = 0; i != nof_re_layer; ++i) {
    // Apply layer mapping for codeword 0.
    for (unsigned layer = 0; layer != nof_layers; ++layer) {
      temp_re.get_slice(layer)[i] = pdsch_symbols[nof_layers * i + layer];
    }
  }

  // Map resource elements.
  map(mapper, temp_re, config);
}
