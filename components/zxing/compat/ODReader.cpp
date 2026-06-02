// SPDX-FileCopyrightText: 2016 Nu-book Inc.
// SPDX-FileCopyrightText: 2016 ZXing authors
// SPDX-FileCopyrightText: 2020 Axel Waggershauser
//
// SPDX-License-Identifier: Apache-2.0

#include "oned/ODReader.h"

#include "BarcodeData.h"
#include "BinaryBitmap.h"
#include "ReaderOptions.h"
#include "oned/ODCodabarReader.h"
#include "oned/ODCode128Reader.h"
#include "oned/ODCode39Reader.h"
#include "oned/ODCode93Reader.h"
#include "oned/ODDataBarExpandedReader.h"
#include "oned/ODDataBarLimitedReader.h"
#include "oned/ODDataBarReader.h"
#include "oned/ODITFReader.h"
#include "oned/ODMultiUPCEANReader.h"

#include <algorithm>
#include <utility>

#ifdef PRINT_DEBUG
#include "BitMatrix.h"
#include "BitMatrixIO.h"
#endif

namespace ZXing::OneD {

Reader::Reader(const ReaderOptions &opts) : ZXing::Reader(opts)
{
    using enum BarcodeFormat;

    _readers.reserve(8);

    if (opts.hasAnyFormat(EANUPC)) {
        _readers.emplace_back(new MultiUPCEANReader(opts));
    }

    if (opts.hasAnyFormat(Code39)) {
        _readers.emplace_back(new Code39Reader(opts));
    }
    if (opts.hasAnyFormat(Code93)) {
        _readers.emplace_back(new Code93Reader(opts));
    }
    if (opts.hasAnyFormat(Code128)) {
        _readers.emplace_back(new Code128Reader(opts));
    }
    if (opts.hasAnyFormat(ITF)) {
        _readers.emplace_back(new ITFReader(opts));
    }
    if (opts.hasAnyFormat(Codabar)) {
        _readers.emplace_back(new CodabarReader(opts));
    }
    if (opts.hasFormat(DataBar | DataBarOmni | DataBarStk | DataBarStkOmni)) {
        _readers.emplace_back(new DataBarReader(opts));
    }
    if (opts.hasFormat(DataBar | DataBarExp | DataBarExpStk)) {
        _readers.emplace_back(new DataBarExpandedReader(opts));
    }
    if (opts.hasFormat(DataBar | DataBarLtd)) {
        _readers.emplace_back(new DataBarLimitedReader(opts));
    }
}

Reader::~Reader() = default;

BarcodesData DoDecode(const std::vector<std::unique_ptr<RowReader>> &readers, const BinaryBitmap &image, bool tryHarder,
                      bool rotate, bool isPure, int maxSymbols, int minLineCount, bool returnErrors)
{
    BarcodesData res;

    std::vector<std::unique_ptr<RowReader::DecodingState>> decodingState(readers.size());

    int width = image.width();
    int height = image.height();

    if (rotate) {
        std::swap(width, height);
    }

    int middle = height / 2;
    int rowStep = std::max(1, height / ((tryHarder && !isPure) ? (maxSymbols == 1 ? 256 : 512) : 32));
    int maxLines = tryHarder ? height : 15;

    if (isPure) {
        minLineCount = 1;
    } else {
        minLineCount = std::min(minLineCount, height);
    }
    std::vector<int> checkRows;

    PatternRow bars;
    bars.reserve(128);

#ifdef PRINT_DEBUG
    BitMatrix dbg(width, height);
#endif

    for (int i = 0; i < maxLines; i++) {
        int rowStepsAboveOrBelow = (i + 1) / 2;
        bool isAbove = (i & 0x01) == 0;
        int rowNumber = middle + rowStep * (isAbove ? rowStepsAboveOrBelow : -rowStepsAboveOrBelow);
        bool isCheckRow = false;
        if (rowNumber < 0 || rowNumber >= height) {
            break;
        }

        if (checkRows.size()) {
            --i;
            rowNumber = checkRows.back();
            checkRows.pop_back();
            isCheckRow = true;
            if (rowNumber < 0 || rowNumber >= height) {
                continue;
            }
        }

        if (!image.getPatternRow(rowNumber, rotate ? 90 : 0, bars)) {
            continue;
        }

#ifdef PRINT_DEBUG
        bool val = false;
        int x = 0;
        for (auto b : bars) {
            for (unsigned j = 0; j < b; ++j) {
                dbg.set(x++, rowNumber, val);
            }
            val = !val;
        }
#endif

        for (bool upsideDown : {
                    false, true
                }) {
            if (upsideDown) {
                std::reverse(bars.begin(), bars.end());
            }
            for (size_t r = 0; r < readers.size(); ++r) {
                if (isPure && i && !decodingState[r]) {
                    continue;
                }

                PatternView next(bars);
                do {
                    BarcodeData result = readers[r]->decodePattern(rowNumber, next, decodingState[r]);
                    if (result.isValid() || (returnErrors && result.error)) {
                        result.lineCount++;
                        if (upsideDown) {
                            for (auto &p : result.position) {
                                p = {width - p.x - 1, p.y};
                            }
                        }
                        if (rotate) {
                            for (auto &p : result.position) {
                                p = {p.y, width - p.x - 1};
                            }
                        }

                        for (auto &other : res) {
                            if (result == other) {
                                auto dTop = maxAbsComponent(other.position.topLeft() - result.position.topLeft());
                                auto dBot = maxAbsComponent(other.position.bottomLeft() - result.position.topLeft());
                                if (dTop < dBot || (dTop == dBot && rotate ^ (sumAbsComponent(other.position[0])
                                                                              > sumAbsComponent(result.position[0])))) {
                                    other.position[0] = result.position[0];
                                    other.position[1] = result.position[1];
                                } else {
                                    other.position[2] = result.position[2];
                                    other.position[3] = result.position[3];
                                }
                                other.lineCount++;
                                result = BarcodeData();
                                break;
                            }
                        }

                        if (result.format != BarcodeFormat::None) {
                            res.push_back(std::move(result));

                            if (!isCheckRow && minLineCount > 1 && rowStep > 1) {
                                checkRows = {rowNumber - 1, rowNumber + 1};
                                if (rowStep > 2) {
                                    checkRows.insert(checkRows.end(), {rowNumber - 2, rowNumber + 2});
                                }
                            }
                        }

                        if (maxSymbols && Reduce(res, 0, [&](int s, const BarcodeData & r) {
                        return s + (r.lineCount >= minLineCount);
                        }) == maxSymbols) {
                            goto out;
                        }
                    }
                    next.shift(2 - (next.index() % 2));
                    next.extend();
                } while (tryHarder && next.size());
            }
        }
    }

out:
    std::erase_if(res, [&](auto &&r) {
        return r.lineCount < minLineCount;
    });

    for (auto a = res.begin(); a != res.end(); ++a) {
        for (auto b = std::next(a); b != res.end(); ++b) {
            if (HaveIntersectingBoundingBoxes(a->position, b->position)) {
                *(a->lineCount < b->lineCount ? a : b) = BarcodeData();
            }
        }
    }

    std::erase_if(res, [](auto &&r) {
        return r.format == BarcodeFormat::None;
    });

#ifdef PRINT_DEBUG
    SaveAsPBM(dbg, rotate ? "od-log-r.pnm" : "od-log.pnm");
#endif

    return res;
}

BarcodesData Reader::read(const BinaryBitmap &image, int maxSymbols) const
{
    auto resH = DoDecode(_readers, image, _opts.tryHarder(), false, _opts.isPure(), maxSymbols, _opts.minLineCount(),
                         _opts.returnErrors());
    if ((!maxSymbols || Size(resH) < maxSymbols) && _opts.tryRotate()) {
        auto resV = DoDecode(_readers, image, _opts.tryHarder(), true, _opts.isPure(), maxSymbols - Size(resH),
                             _opts.minLineCount(), _opts.returnErrors());
        resH.insert(resH.end(), std::make_move_iterator(resV.begin()), std::make_move_iterator(resV.end()));
    }
    return resH;
}

} // namespace ZXing::OneD
