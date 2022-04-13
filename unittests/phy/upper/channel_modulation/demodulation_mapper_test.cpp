/// \file
/// \brief Demodulation mapper unit test.
///
/// The test takes as input vectors containing noisy modulated symbols and the corresponding noise variances. The
/// symbols are demodulated and the resulting bits (both soft and hard versions) are compared with the expected values,
/// also provided by test vectors.

#include "demodulation_mapper_test_data.h"
#include "srsgnb/adt/static_vector.h"
#include "srsgnb/phy/upper/channel_modulation/demodulation_mapper.h"
#include "srsgnb/phy/upper/channel_modulation/modulation_mapper.h"

#include <random>

using namespace srsgnb;

int main()
{
  std::unique_ptr<demodulation_mapper> demodulator = create_demodulation_mapper();

  for (const auto& test_case : demodulation_mapper_test_data) {
    const modulation_scheme mod = test_case.scheme;

    // For now, we can only demodulate up to 16QAM
    if (mod > modulation_scheme::QAM16) {
      continue;
    }

    const unsigned          nof_symbols = test_case.nsymbols;
    const std::vector<cf_t> symbols     = test_case.symbols.read();
    srsran_assert(symbols.size() == nof_symbols, "Error reading modulated symbols.");

    const std::vector<float> noise_var = test_case.noise_var.read();
    srsran_assert(noise_var.size() == nof_symbols, "Error reading noise variances.");
    srsran_assert(std::all_of(noise_var.cbegin(), noise_var.cend(), [](float f) { return f > 0; }),
                  "Noise variances should take positive values.");

    const unsigned      nof_bits = nof_symbols * static_cast<unsigned>(mod);
    std::vector<int8_t> soft_bits(nof_bits);
    demodulator->demodulate_soft(soft_bits, symbols, noise_var, mod);

    const std::vector<int8_t> soft_bits_true = test_case.soft_bits.read();
    srsran_assert(soft_bits_true.size() == nof_bits, "Error reading soft bits.");

    srsran_assert(
        std::equal(
            soft_bits.cbegin(), soft_bits.cend(), soft_bits_true.cbegin(), [](int8_t a, int8_t b) { return a == b; }),
        "Soft bits are not sufficiently precise.");

    std::vector<uint8_t> hard_bits(nof_bits);
    std::transform(soft_bits.cbegin(), soft_bits.cend(), hard_bits.begin(), [](int8_t a) { return (a > 0) ? 0U : 1U; });
    const std::vector<uint8_t> hard_bits_true = test_case.hard_bits.read();
    srsran_assert(hard_bits_true.size() == nof_bits, "Error reading hard bits.");
    srsran_assert(std::equal(hard_bits.cbegin(), hard_bits.cend(), hard_bits_true.cbegin()), "Hard bits do not match.");
  }
}