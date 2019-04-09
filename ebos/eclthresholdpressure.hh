// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/
/*!
 * \file
 *
 * \copydoc Ewoms::EclThresholdPressure
 */
#ifndef EWOMS_ECL_THRESHOLD_PRESSURE_HH
#define EWOMS_ECL_THRESHOLD_PRESSURE_HH

#include <ewoms/common/propertysystem.hh>

#include <opm/material/densead/Evaluation.hpp>
#include <opm/material/densead/Math.hpp>

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/GridProperty.hpp>
#include <opm/parser/eclipse/EclipseState/Tables/Eqldims.hpp>
#include <opm/parser/eclipse/EclipseState/SimulationConfig/SimulationConfig.hpp>
#include <opm/parser/eclipse/EclipseState/SimulationConfig/ThresholdPressure.hpp>

#include <opm/material/common/Exceptions.hpp>

#include <dune/grid/common/gridenums.hh>
#include <dune/common/version.hh>

#include <array>
#include <vector>
#include <unordered_map>

BEGIN_PROPERTIES

NEW_PROP_TAG(Simulator);
NEW_PROP_TAG(Scalar);
NEW_PROP_TAG(Evaluation);
NEW_PROP_TAG(ElementContext);
NEW_PROP_TAG(FluidSystem);
NEW_PROP_TAG(EnableExperiments);

END_PROPERTIES

namespace Ewoms {

/*!
 * \ingroup EclBlackOilSimulator
 *
 * \brief This class calculates the threshold pressure for grid faces according to the
 *        Eclipse Reference Manual.
 *
 * If the difference of the pressure potential between two cells is below the threshold
 * pressure, the pressure potential difference is assumed to be zero, if it is larger
 * than the threshold pressure, it is reduced by the threshold pressure.
 */
template <class TypeTag>
class EclThresholdPressure
{
    typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, Evaluation) Evaluation;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;
    typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;

    enum { enableExperiments = GET_PROP_VALUE(TypeTag, EnableExperiments) };
    enum { numPhases = FluidSystem::numPhases };

public:
    EclThresholdPressure(const Simulator& simulator)
        : simulator_(simulator)
    {
        enableThresholdPressure_ = false;
    }


    void setFromRestart(const std::vector<Scalar>& values)
    { thpres_ = values; }

    /*!
     * \brief Actually compute the threshold pressures over a face as a pre-compute step.
     */
    void finishInit()
    {
        const auto& gridView = simulator_.gridView();

        unsigned numElements = gridView.size(/*codim=*/0);

        // this code assumes that the DOFs are the elements. (i.e., an
        // ECFV spatial discretization with TPFA). if you try to use
        // it with something else, you're currently out of luck,
        // sorry!
        assert(simulator_.model().numGridDof() == numElements);

        const auto& vanguard = simulator_.vanguard();
        const auto& eclState = vanguard.eclState();
        const auto& simConfig = eclState.getSimulationConfig();

        enableThresholdPressure_ = simConfig.useThresholdPressure();
        if (!enableThresholdPressure_)
            return;

        numEquilRegions_ = eclState.getTableManager().getEqldims().getNumEquilRegions();
        if (numEquilRegions_ > 0xff) {
            // make sure that the index of an equilibration region can be stored in a
            // single byte
            throw std::runtime_error("The maximum number of supported equilibration regions is 255!");
        }

        // internalize the data specified using the EQLNUM keyword
        const std::vector<int>& equilRegionData =
            eclState.get3DProperties().getIntGridProperty("EQLNUM").getData();
        elemEquilRegion_.resize(numElements, 0);
        for (unsigned elemIdx = 0; elemIdx < numElements; ++elemIdx) {
            int cartElemIdx = vanguard.cartesianIndex(elemIdx);

            // ECL uses Fortran-style indices but we want C-style ones!
            elemEquilRegion_[elemIdx] = equilRegionData[cartElemIdx] - 1;
        }

        /*
          If this is a restart run the ThresholdPressure object will be active,
          but it will *not* be properly initialized with numerical values. The
          values must instead come from the THPRES vector in the restart file.
        */
        if (simConfig.getThresholdPressure().restart())
            return;

        // allocate the array which specifies the threshold pressures
        thpres_.resize(numEquilRegions_*numEquilRegions_, 0.0);
        thpresDefault_.resize(numEquilRegions_*numEquilRegions_, 0.0);

        computeDefaultThresholdPressures_();
        applyExplicitThresholdPressures_();
    }

    /*!
     * \brief Returns the theshold pressure [Pa] for the intersection between two elements.
     *
     * This is tailor made for the E100 threshold pressure mechanism and it is thus quite
     * a hack: First of all threshold pressures in general are unphysical, and second,
     * they should be different for the fluid phase but are not. Anyway, this seems to be
     * E100's way of doing things, so we do it the same way.
     */
    Scalar thresholdPressure(int elem1Idx, int elem2Idx) const
    {
        if (!enableThresholdPressure_)
            return 0.0;

        if (enableExperiments) {
            // threshold pressure accross faults
            if (!thpresftValues_.empty()) {
                const auto& vanguard = simulator_.vanguard();
                int cartElem1Idx = vanguard.cartesianIndex(elem1Idx);
                int cartElem2Idx = vanguard.cartesianIndex(elem2Idx);

                assert(0 <= cartElem1Idx && cartElemFaultIdx_.size() > cartElem1Idx);
                assert(0 <= cartElem2Idx && cartElemFaultIdx_.size() > cartElem2Idx);

                int fault1Idx = cartElemFaultIdx_[cartElem1Idx];
                int fault2Idx = cartElemFaultIdx_[cartElem2Idx];
                if (fault1Idx != -1 && fault1Idx == fault2Idx)
                    // inside a fault there's no threshold pressure, even accross EQUIL
                    // regions.
                    return 0.0;
                if (fault1Idx != fault2Idx) {
                    // TODO: which value if a cell is part of multiple faults? we take
                    // the maximum here.
                    Scalar val1 = (fault1Idx >= 0) ? thpresftValues_[fault1Idx] : 0.0;
                    Scalar val2 = (fault2Idx >= 0) ? thpresftValues_[fault2Idx] : 0.0;
                    return std::max(val1, val2);
                }
            }
        }

        // threshold pressure accross EQUIL regions
        unsigned short equilRegion1Idx = elemEquilRegion_[elem1Idx];
        unsigned short equilRegion2Idx = elemEquilRegion_[elem2Idx];

        if (equilRegion1Idx == equilRegion2Idx)
            return 0.0;

        return thpres_[equilRegion1Idx*numEquilRegions_ + equilRegion2Idx];
    }

    const std::vector<Scalar>& data() const {
        return thpres_;
    }


private:
    // compute the defaults of the threshold pressures using the initial condition
    void computeDefaultThresholdPressures_()
    {
        const auto& vanguard = simulator_.vanguard();
        const auto& gridView = vanguard.gridView();

        typedef Opm::MathToolbox<Evaluation> Toolbox;
        // loop over the whole grid and compute the maximum gravity adjusted pressure
        // difference between two EQUIL regions.
        auto elemIt = gridView.template begin</*codim=*/ 0>();
        const auto& elemEndIt = gridView.template end</*codim=*/ 0>();
        ElementContext elemCtx(simulator_);
        for (; elemIt != elemEndIt; ++elemIt) {

            const auto& elem = *elemIt;
            if (elem.partitionType() != Dune::InteriorEntity)
                continue;

            elemCtx.updateAll(elem);
            const auto& stencil = elemCtx.stencil(/*timeIdx=*/0);

            for (unsigned scvfIdx = 0; scvfIdx < stencil.numInteriorFaces(); ++ scvfIdx) {
                const auto& face = stencil.interiorFace(scvfIdx);

                unsigned i = face.interiorIndex();
                unsigned j = face.exteriorIndex();

                unsigned insideElemIdx = elemCtx.globalSpaceIndex(i, /*timeIdx=*/0);
                unsigned outsideElemIdx = elemCtx.globalSpaceIndex(j, /*timeIdx=*/0);

                unsigned equilRegionInside = elemEquilRegion_[insideElemIdx];
                unsigned equilRegionOutside = elemEquilRegion_[outsideElemIdx];

                if (equilRegionInside == equilRegionOutside)
                    // the current face is not at the boundary between EQUIL regions!
                    continue;

                // don't include connections with negligible flow
                const Evaluation& trans = simulator_.problem().transmissibility(elemCtx, i, j);
                Scalar faceArea = face.area();
                if (std::abs(faceArea*Opm::getValue(trans)) < 1e-18)
                    continue;

                // determine the maximum difference of the pressure of any phase over the
                // intersection
                Scalar pth = 0.0;
                const auto& extQuants = elemCtx.extensiveQuantities(scvfIdx, /*timeIdx=*/0);
                for (unsigned phaseIdx = 0; phaseIdx < numPhases; ++phaseIdx) {
                    unsigned upIdx = extQuants.upstreamIndex(phaseIdx);
                    const auto& up = elemCtx.intensiveQuantities(upIdx, /*timeIdx=*/0);

                    if (up.mobility(phaseIdx) > 0.0) {
                        Scalar phaseVal = Toolbox::value(extQuants.pressureDifference(phaseIdx));
                        pth = std::max(pth, std::abs(phaseVal));
                    }
                }

                int offset1 = equilRegionInside*numEquilRegions_ + equilRegionOutside;
                int offset2 = equilRegionOutside*numEquilRegions_ + equilRegionInside;

                thpresDefault_[offset1] = std::max(thpresDefault_[offset1], pth);
                thpresDefault_[offset2] = std::max(thpresDefault_[offset2], pth);
            }
        }

        // make sure that the threshold pressures is consistent for parallel
        // runs. (i.e. take the maximum of all processes)
        for (unsigned i = 0; i < thpresDefault_.size(); ++i)
            thpresDefault_[i] = gridView.comm().max(thpresDefault_[i]);
    }

    // internalize the threshold pressures which where explicitly specified via the
    // THPRES keyword.
    void applyExplicitThresholdPressures_()
    {
        const auto& vanguard = simulator_.vanguard();
        const auto& gridView = vanguard.gridView();
        const auto& elementMapper = simulator_.model().elementMapper();
        const auto& eclState = simulator_.vanguard().eclState();
        const auto& deck = simulator_.vanguard().deck();
        const Opm::SimulationConfig& simConfig = eclState.getSimulationConfig();
        const auto& thpres = simConfig.getThresholdPressure();

        // set the threshold pressures for all EQUIL region boundaries which have a
        // intersection in the grid
        auto elemIt = gridView.template begin</*codim=*/ 0>();
        const auto& elemEndIt = gridView.template end</*codim=*/ 0>();
        for (; elemIt != elemEndIt; ++elemIt) {
            const auto& elem = *elemIt;
            if (elem.partitionType() != Dune::InteriorEntity)
                continue;

            auto isIt = gridView.ibegin(elem);
            const auto& isEndIt = gridView.iend(elem);
            for (; isIt != isEndIt; ++ isIt) {
                // store intersection, this might be costly
                const auto& intersection = *isIt;

                // ignore boundary intersections for now (TODO?)
                if (intersection.boundary())
                    continue;

                const auto& inside = intersection.inside();
                const auto& outside = intersection.outside();

                unsigned insideElemIdx = elementMapper.index(inside);
                unsigned outsideElemIdx = elementMapper.index(outside);

                unsigned equilRegionInside = elemEquilRegion_[insideElemIdx];
                unsigned equilRegionOutside = elemEquilRegion_[outsideElemIdx];
                if (thpres.hasRegionBarrier(equilRegionInside + 1, equilRegionOutside + 1)) {
                    Scalar pth = 0.0;
                    if (thpres.hasThresholdPressure(equilRegionInside + 1, equilRegionOutside + 1)) {
                        // threshold pressure explicitly specified
                        pth = thpres.getThresholdPressure(equilRegionInside + 1, equilRegionOutside + 1);
                    }
                    else {
                        // take the threshold pressure from the initial condition
                        unsigned offset = equilRegionInside*numEquilRegions_ + equilRegionOutside;
                        pth = thpresDefault_[offset];
                    }

                    unsigned offset1 = equilRegionInside*numEquilRegions_ + equilRegionOutside;
                    unsigned offset2 = equilRegionOutside*numEquilRegions_ + equilRegionInside;

                    thpres_[offset1] = pth;
                    thpres_[offset2] = pth;
                }
            }
        }

        if (enableExperiments) {
            // apply threshold pressures accross faults (experimental!)
            if (deck.hasKeyword("THPRESFT"))
                extractThpresft_(deck.getKeyword("THPRESFT"));
        }

    }

    void extractThpresft_(const Opm::DeckKeyword& thpresftKeyword)
    {
        // retrieve the faults collection.
        const Opm::EclipseState& eclState = simulator_.vanguard().eclState();
        const Opm::FaultCollection& faults = eclState.getFaults();

        // extract the multipliers from the deck keyword
        int numFaults = faults.size();
        int numCartesianElem = eclState.getInputGrid().getCartesianSize();
        thpresftValues_.resize(numFaults, -1.0);
        cartElemFaultIdx_.resize(numCartesianElem, -1);
        for (size_t recordIdx = 0; recordIdx < thpresftKeyword.size(); ++ recordIdx) {
            const Opm::DeckRecord& record = thpresftKeyword.getRecord(recordIdx);

            const std::string& faultName = record.getItem("FAULT_NAME").getTrimmedString(0);
            Scalar thpresValue = record.getItem("VALUE").getSIDouble(0);

            for (size_t faultIdx = 0; faultIdx < faults.size(); faultIdx++) {
                auto& fault = faults.getFault(faultIdx);
                if (fault.getName() != faultName)
                    continue;

                thpresftValues_[faultIdx] = thpresValue;
                for (const Opm::FaultFace& face: fault)
                    // "face" is a misnomer because the object describes a set of cell
                    // indices, but we go with the conventions of the parser here...
                    for (size_t cartElemIdx: face)
                        cartElemFaultIdx_[cartElemIdx] = faultIdx;
            }
        }
    }

    const Simulator& simulator_;

    std::vector<Scalar> thpresDefault_;
    std::vector<Scalar> thpres_;
    unsigned numEquilRegions_;
    std::vector<unsigned char> elemEquilRegion_;

    // threshold pressure accross faults. EXPERIMENTAL!
    std::vector<Scalar> thpresftValues_;
    std::vector<int> cartElemFaultIdx_;

    bool enableThresholdPressure_;
};

} // namespace Ewoms

#endif
