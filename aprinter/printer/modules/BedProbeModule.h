/*
 * Copyright (c) 2015 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef APRINTER_BED_PROBE_H
#define APRINTER_BED_PROBE_H

#include <stdint.h>
#include <math.h>

#include <aprinter/meta/TypeListUtils.h>
#include <aprinter/meta/StructIf.h>
#include <aprinter/meta/ListForEach.h>
#include <aprinter/meta/BasicMetaUtils.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/WrapFunction.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/ProgramMemory.h>
#include <aprinter/base/Hints.h>
#include <aprinter/base/LoopUtils.h>
#include <aprinter/math/Matrix.h>
#include <aprinter/math/LinearLeastSquares.h>
#include <aprinter/printer/Configuration.h>
#include <aprinter/printer/ServiceList.h>
#include <aprinter/printer/HookExecutor.h>
#include <aprinter/printer/utils/JsonBuilder.h>
#include <aprinter/printer/utils/ModuleUtils.h>

namespace APrinter {

template <typename ModuleArg>
class BedProbeModule {
    APRINTER_UNPACK_MODULE_ARG(ModuleArg)
    
public:
    struct Object;
    
private:
    using ProbePoints = typename Params::ProbePoints;
    using PlatformAxesList = typename Params::PlatformAxesList;
    using CorrectionParams = typename Params::ProbeCorrectionParams;
    static const int NumPoints = TypeListLength<ProbePoints>::Value;
    static const int NumPlatformAxes = TypeListLength<PlatformAxesList>::Value;
    using PointIndexType = ChooseIntForMax<NumPoints, true>;
    
    using Config = typename ThePrinterMain::Config;
    using TheCommand = typename ThePrinterMain::TheCommand;
    using FpType = typename ThePrinterMain::FpType;
    static const int ProbeAxisIndex = ThePrinterMain::template FindPhysVirtAxis<Params::ProbeAxis>::Value;
    
    struct BedProbeHookCompletedHandler;
    
public:
    AMBRO_STRUCT_IF(CorrectionFeature, CorrectionParams::Enabled) {
        friend BedProbeModule;
        static_assert(ThePrinterMain::IsTransformEnabled, "");
        static_assert(ThePrinterMain::template IsVirtAxis<ProbeAxisIndex>::Value, "");
        
    private:
        static bool const QuadraticSupported = CorrectionParams::QuadraticCorrectionSupported;
        static int const NumBaseFactors = NumPlatformAxes + 1;
        static int const NumQuadraticFactors = QuadraticSupported ? (NumPlatformAxes * (NumPlatformAxes + 1) / 2) : 0;
        static int const MaxCorrectionFactors = NumBaseFactors + NumQuadraticFactors;
        
        using CorrectionsMatrix = Matrix<FpType, MaxCorrectionFactors, 1>;
        using LeastSquaresMatrix = Matrix<FpType, NumPoints, MaxCorrectionFactors>;
        
        AMBRO_STRUCT_IF(QuadraticFeature, QuadraticSupported) {
            using CQuadraticCorrectionEnabled = decltype(ExprCast<bool>(Config::e(CorrectionParams::QuadraticCorrectionEnabled::i())));
            using ConfigExprs = MakeTypeList<CQuadraticCorrectionEnabled>;
            
            static bool quadratic_enabled (Context c)
            {
                return APRINTER_CFG(Config, CQuadraticCorrectionEnabled, c);
            }
            
            static void print_quadratic_corrections (Context c, TheCommand *cmd, CorrectionsMatrix const *corrections)
            {
                ListFor<QuadraticFactorHelperList>([&] APRINTER_TL(helper, helper::print_quadratic_corrections(c, cmd, corrections)));
            }
            
            static void add_quadratic_factors_to_row (Context c, LeastSquaresMatrix *matrix, int row)
            {
                if (quadratic_enabled(c)) {
                    ListFor<QuadraticFactorHelperList>([&] APRINTER_TL(helper, helper::add_quadratic_factors_to_row(c, matrix, row)));
                }
            }
            
            static int get_num_quadratic_columns (Context c)
            {
                return quadratic_enabled(c) ? NumQuadraticFactors : 0;
            }
            
            template <typename Src>
            static FpType compute_quadratic_correction_for_point (Context c, Src src, CorrectionsMatrix const *corrections)
            {
                return ListForFold<QuadraticFactorHelperList>(0.0f, [&] APRINTER_TLA(helper, (FpType accum), return helper::compute_quadratic_correction_for_point(accum, c, src, corrections)));
            }
            
            struct BaseIndices {
                static int const PlatformAxisIndex1 = 0;
                static int const PlatformAxisIndex2 = -1;
            };
            
            template <int FactorIndex>
            struct QuadraticFactorHelper {
                using PrevIndices = If<(FactorIndex == 0), BaseIndices, QuadraticFactorHelper<(FactorIndex - 1)>>;
                static bool const NextAxis1 = PrevIndices::PlatformAxisIndex2 == NumPlatformAxes - 1;
                static int const PlatformAxisIndex1 = PrevIndices::PlatformAxisIndex1 + NextAxis1;
                static int const PlatformAxisIndex2 = NextAxis1 ? PlatformAxisIndex1 : (PrevIndices::PlatformAxisIndex2 + 1);
                static int const MatrixFactorIndex = NumBaseFactors + FactorIndex;
                
                static void print_quadratic_corrections (Context c, TheCommand *cmd, CorrectionsMatrix const *corrections)
                {
                    cmd->reply_append_ch(c, ' ');
                    cmd->reply_append_ch(c, AxisHelper<PlatformAxisIndex1>::AxisName);
                    cmd->reply_append_ch(c, AxisHelper<PlatformAxisIndex2>::AxisName);
                    cmd->reply_append_ch(c, ':');
                    cmd->reply_append_fp(c, (*corrections)++(MatrixFactorIndex, 0));
                }
                
                static void add_quadratic_factors_to_row (Context c, LeastSquaresMatrix *matrix, int row)
                {
                    (*matrix)--(row, MatrixFactorIndex) = (*matrix)++(row, PlatformAxisIndex1) * (*matrix)++(row, PlatformAxisIndex2);
                }
                
                template <typename Src>
                static FpType compute_quadratic_correction_for_point (FpType accum, Context c, Src src, CorrectionsMatrix const *corrections)
                {
                    FpType coord1 = src.template get<AxisHelper<PlatformAxisIndex1>::VirtAxisIndex()>();
                    FpType coord2 = src.template get<AxisHelper<PlatformAxisIndex2>::VirtAxisIndex()>();
                    return accum + (coord1 * coord2) * (*corrections)++(MatrixFactorIndex, 0);
                }
            };
            using QuadraticFactorHelperList = IndexElemListCount<NumQuadraticFactors, QuadraticFactorHelper>;
        }
        AMBRO_STRUCT_ELSE(QuadraticFeature) {
            static void print_quadratic_corrections (Context c, TheCommand *cmd, CorrectionsMatrix const *corrections) {}
            static void add_quadratic_factors_to_row (Context c, LeastSquaresMatrix *matrix, int row) {}
            static int get_num_quadratic_columns (Context c) { return 0; }
            template <typename Src>
            static FpType compute_quadratic_correction_for_point (Context c, Src src, CorrectionsMatrix const *corrections) { return 0.0f; }
            using ConfigExprs = EmptyTypeList;
        };
        
        static void init (Context c)
        {
            auto *o = Object::self(c);
            MatrixWriteZero(o->corrections--);
        }
        
        static void apply_corrections (Context c)
        {
            ThePrinterMain::TransformFeature::handle_corrections_change(c);
        }
        
        static void print_corrections (Context c, TheCommand *cmd, CorrectionsMatrix const *corrections, AMBRO_PGM_P msg)
        {
            cmd->reply_append_pstr(c, msg);
            
            cmd->reply_append_ch(c, ' ');
            cmd->reply_append_ch(c, ThePrinterMain::template PhysVirtAxisHelper<ProbeAxisIndex>::AxisName);
            cmd->reply_append_ch(c, ':');
            cmd->reply_append_fp(c, (*corrections)++(NumPlatformAxes, 0));
            
            ListFor<AxisHelperList>([&] APRINTER_TL(helper, helper::print_correction(c, cmd, corrections)));
            
            QuadraticFeature::print_quadratic_corrections(c, cmd, corrections);
            
            cmd->reply_append_ch(c, '\n');
        }
        
        static bool check_command (Context c, TheCommand *cmd)
        {
            auto *o = Object::self(c);
            if (cmd->getCmdNumber(c) == 937) {
                print_corrections(c, cmd, &o->corrections, AMBRO_PSTR("EffectiveCorrections"));
                cmd->finishCommand(c);
                return false;
            }
            if (cmd->getCmdNumber(c) == 561) {
                if (!cmd->tryUnplannedCommand(c)) {
                    return false;
                }
                MatrixWriteZero(o->corrections--);
                apply_corrections(c);
                cmd->finishCommand(c);
                return false;
            }
            return true;
        }
        
        static void probing_staring (Context c)
        {
            auto *o = Object::self(c);
            for (auto i : LoopRange<PointIndexType>(NumPoints)) {
                o->heights_matrix--(i, 0) = NAN;
            }
        }
        
        static void probing_measurement (Context c, PointIndexType point_index, FpType height)
        {
            auto *o = Object::self(c);
            o->heights_matrix--(point_index, 0) = height;
        }
        
        static bool probing_completing (Context c, TheCommand *cmd)
        {
            auto *o = Object::self(c);
            
            LeastSquaresMatrix coordinates_matrix;
            
            ListFor<AxisHelperList>([&] APRINTER_TL(helper, helper::fill_point_coordinates(c, coordinates_matrix--)));
            
            int num_columns = NumBaseFactors + QuadraticFeature::get_num_quadratic_columns(c);
            
            PointIndexType num_valid_points = 0;
            
            for (auto i : LoopRange<PointIndexType>(NumPoints)) {
                if (isnan(o->heights_matrix--(i, 0))) {
                    continue;
                }
                
                coordinates_matrix--(i, NumPlatformAxes) = 1.0f;
                QuadraticFeature::add_quadratic_factors_to_row(c, &coordinates_matrix, i);
                
                if (i != num_valid_points) {
                    MatrixCopy(coordinates_matrix--.range(num_valid_points, 0, 1, num_columns), coordinates_matrix++.range(i, 0, 1, num_columns));
                    MatrixCopy(o->heights_matrix--.range(num_valid_points, 0, 1, 1), o->heights_matrix++.range(i, 0, 1, 1));
                }
                
                num_valid_points++;
            }
            
            if (num_valid_points < num_columns) {
                cmd->reportError(c, AMBRO_PSTR("TooFewPointsForCorrection"));
                return false;
            }
            
            auto effective_coordinates_matrix = coordinates_matrix--.range(0, 0, num_valid_points, num_columns);
            auto effective_heights_matrix = o->heights_matrix--.range(0, 0, num_valid_points, 1);
            
            CorrectionsMatrix new_corrections;
            
            LinearLeastSquaresMaxSize<NumPoints, MaxCorrectionFactors>(effective_coordinates_matrix--, effective_heights_matrix++, new_corrections--.range(0, 0, num_columns, 1));
            MatrixWriteZero(new_corrections--.range(num_columns, 0, MaxCorrectionFactors - num_columns, 1));
            
            print_corrections(c, cmd, &new_corrections, AMBRO_PSTR("RelativeCorrections"));
            
            bool bad_corrections = false;
            for (auto i : LoopRange<int>(MaxCorrectionFactors)) {
                FpType x = new_corrections++(i, 0);
                if (isnan(x) || isinf(x)) {
                    bad_corrections = true;
                }
            }
            
            if (bad_corrections) {
                cmd->reportError(c, AMBRO_PSTR("BadCorrections"));
                return false;
            }
            
            if (!cmd->find_command_param(c, 'D', nullptr)) {
                MatrixElemOpInPlace<MatrixElemOpAdd>(o->corrections--, new_corrections++);
                apply_corrections(c);
            }
            
            return true;
        }
        
        template <typename Src>
        static FpType compute_correction_for_point (Context c, Src src)
        {
            auto *o = Object::self(c);
            FpType constant_correction = o->corrections++(NumPlatformAxes, 0);
            FpType linear_correction = ListForFold<AxisHelperList>(0.0f, [&] APRINTER_TLA(helper, (FpType accum), return helper::calc_correction_contribution(accum, c, src, &o->corrections)));
            FpType quadratic_correction = QuadraticFeature::compute_quadratic_correction_for_point(c, src, &o->corrections);
            return constant_correction + linear_correction + quadratic_correction;
        }
        
        template <int VirtAxisIndex>
        struct VirtAxisHelper {
            template <typename Src, typename Dst, bool Reverse>
            static void correct_virt_axis (Context c, Src src, Dst dst, FpType correction_value, WrapBool<Reverse>)
            {
                FpType coord_value = src.template get<VirtAxisIndex>();
                if (VirtAxisIndex == ThePrinterMain::template GetVirtAxisVirtIndex<ProbeAxisIndex>::Value) {
                    if (Reverse) {
                        coord_value -= correction_value;
                    } else {
                        coord_value += correction_value;
                    }
                }
                dst.template set<VirtAxisIndex>(coord_value);
            }
        };
        using VirtAxisHelperList = IndexElemListCount<ThePrinterMain::TransformFeature::NumVirtAxes, VirtAxisHelper>;
        
    public:
        static bool const CorrectionEnabled = true;
        
        template <typename Src, typename Dst, bool Reverse>
        static void do_correction (Context c, Src src, Dst dst, WrapBool<Reverse>)
        {
            FpType correction_value = compute_correction_for_point(c, src);
            ListFor<VirtAxisHelperList>([&] APRINTER_TL(helper, helper::correct_virt_axis(c, src, dst, correction_value, WrapBool<Reverse>())));
        }
        
    public:
        using ConfigExprs = typename QuadraticFeature::ConfigExprs;
        
        struct Object : public ObjBase<CorrectionFeature, typename BedProbeModule::Object, EmptyTypeList> {
            Matrix<FpType, NumPoints, 1> heights_matrix;
            CorrectionsMatrix corrections;
        };
    } AMBRO_STRUCT_ELSE(CorrectionFeature) {
        static void init (Context c) {}
        static bool check_command (Context c, TheCommand *cmd) { return true; }
        static void probing_staring (Context c) {}
        static void probing_measurement (Context c, PointIndexType point_index, FpType height) {}
        static bool probing_completing (Context c, TheCommand *cmd) { return true; }
        struct Object {};
    };
    
public:
    static void init (Context c)
    {
        auto *o = Object::self(c);
        o->m_current_point = -1;
        Context::Pins::template setInput<typename Params::ProbePin, typename Params::ProbePinInputMode>(c);
        CorrectionFeature::init(c);
    }
    
    static bool check_command (Context c, TheCommand *cmd)
    {
        auto *o = Object::self(c);
        return CorrectionFeature::check_command(c, cmd);
    }
    
    static bool check_g_command (Context c, TheCommand *cmd)
    {
        auto *o = Object::self(c);
        if (cmd->getCmdNumber(c) == 32) {
            if (!cmd->tryUnplannedCommand(c)) {
                return false;
            }
            AMBRO_ASSERT(o->m_current_point == -1)
            uint32_t point_number;
            if (cmd->find_command_param_uint32(c, 'P', &point_number)) {
                if (!(point_number >= 1 && point_number <= NumPoints)) {
                    cmd->reportError(c, AMBRO_PSTR("InvalidPointNumber"));
                    cmd->finishCommand(c);
                    return false;
                }
                o->m_single_point_retract_dist = cmd->get_command_param_fp(c, 'R', 0.0f);
                o->m_current_point = point_number - 1;
                o->m_single_point_mode = true;
            } else {
                o->m_current_point = 0;
                o->m_single_point_mode = false;
                skip_disabled_points_and_detect_end(c);
                if (o->m_current_point == -1) {
                    cmd->reportError(c, AMBRO_PSTR("NoProbePointsEnabled"));
                    cmd->finishCommand(c);
                    return false;
                }
            }
            init_probe_planner(c, false);
            o->m_point_state = 0;
            o->m_command_sent = false;
            o->m_move_error = false;
            CorrectionFeature::probing_staring(c);
            return false;
        }
        return true;
    }
    
    template <typename TheJsonBuilder>
    static void get_json_status (Context c, TheJsonBuilder *json)
    {
        json->addKeyObject(JsonSafeString{"bedProbe"});
        json->endObject();
    }
    
    static void m119_append_endstop (Context c, TheCommand *cmd)
    {
        bool triggered = endstop_is_triggered(c);
        cmd->reply_append_pstr(c, AMBRO_PSTR(" Probe:"));
        cmd->reply_append_ch(c, (triggered ? '1' : '0'));
    }
    
    template <typename CallbackContext>
    AMBRO_ALWAYS_INLINE
    static bool prestep_callback (CallbackContext c)
    {
        return endstop_is_triggered(c);
    }
    
    using HookDefinitionList = MakeTypeList<
        HookDefinition<ServiceList::AfterBedProbingHookService, typename ThePrinterMain::GenericHookDispatcher, BedProbeHookCompletedHandler>
    >;
    
private:
    template <typename ThisContext>
    static bool endstop_is_triggered (ThisContext c)
    {
        return Context::Pins::template get<typename Params::ProbePin>(c) != APRINTER_CFG(Config, CProbeInvert, c);
    }
    
    template <int PointIndex>
    struct PointHelper {
        using Point = TypeListGet<ProbePoints, PointIndex>;
        
        using CPointEnabled = decltype(ExprCast<bool>(Config::e(Point::Enabled::i())));
        using CPointZOffset = decltype(ExprCast<FpType>(Config::e(Point::ZOffset::i())));
        
        template <int PlatformAxisIndex>
        using CPointCoordForAxis = decltype(ExprCast<FpType>(Config::e(TypeListGet<typename Point::Coords, PlatformAxisIndex>::i())));
        using CPointCoordList = IndexElemListCount<NumPlatformAxes, CPointCoordForAxis>;
        
        using ConfigExprs = JoinTypeLists<MakeTypeList<CPointEnabled, CPointZOffset>, CPointCoordList>;
        
        static PointIndexType skip_point_if_disabled (PointIndexType point_index, Context c)
        {
            if (point_index == PointIndex && !APRINTER_CFG(Config, CPointEnabled, c)) {
                point_index++;
            }
            return point_index;
        }
        
        static FpType get_z_offset (Context c)
        {
            return APRINTER_CFG(Config, CPointZOffset, c);
        }
        
        template <int PlatformAxisIndex>
        static FpType get_coord (Context c, WrapInt<PlatformAxisIndex>)
        {
            using CPointCoord = TypeListGet<CPointCoordList, PlatformAxisIndex>;
            return APRINTER_CFG(Config, CPointCoord, c);
        }
        
        struct Object : public ObjBase<PointHelper, typename BedProbeModule::Object, EmptyTypeList> {};
    };
    using PointHelperList = IndexElemList<ProbePoints, PointHelper>;
    
    static FpType get_point_z_offset (Context c, PointIndexType point_index)
    {
        return ListForOne<PointHelperList, 0, FpType>(point_index, [&] APRINTER_TL(helper, return helper::get_z_offset(c)));
    }
    
    template <int PlatformAxisIndex>
    static FpType get_point_coord (Context c, PointIndexType point_index)
    {
        return ListForOne<PointHelperList, 0, FpType>(point_index, [&] APRINTER_TL(helper, return helper::get_coord(c, WrapInt<PlatformAxisIndex>())));
    }
    
    static void skip_disabled_points_and_detect_end (Context c)
    {
        auto *o = Object::self(c);
        o->m_current_point = ListForFold<PointHelperList>(o->m_current_point, [&] APRINTER_TLA(helper, (PointIndexType accum), return helper::skip_point_if_disabled(accum, c)));
        if (o->m_current_point >= NumPoints) {
            o->m_current_point = -1;
        }
    }
    
    template <int PlatformAxisIndex>
    struct AxisHelper {
        struct Object;
        using PlatformAxis = TypeListGet<PlatformAxesList, PlatformAxisIndex>;
        static const int AxisIndex = ThePrinterMain::template FindPhysVirtAxis<PlatformAxis::Value>::Value;
        static const char AxisName = ThePrinterMain::template PhysVirtAxisHelper<AxisIndex>::AxisName;
        using AxisProbeOffset = TypeListGet<typename Params::ProbePlatformOffset, PlatformAxisIndex>;
        
        using CAxisProbeOffset = decltype(ExprCast<FpType>(Config::e(AxisProbeOffset::i())));
        using ConfigExprs = MakeTypeList<CAxisProbeOffset>;
        
        template <typename ReturnType=int>
        static constexpr ReturnType VirtAxisIndex ()
        {
            static_assert(ThePrinterMain::template IsVirtAxis<AxisIndex>::Value, "");
            return ThePrinterMain::template GetVirtAxisVirtIndex<AxisIndex>::Value;
        }
        
        static void add_axis (Context c, PointIndexType point_index)
        {
            FpType coord = get_point_coord<PlatformAxisIndex>(c, point_index);
            ThePrinterMain::template move_add_axis<AxisIndex>(c, coord + APRINTER_CFG(Config, CAxisProbeOffset, c));
        }
        
        static void fill_point_coordinates (Context c, MatrixRange<FpType> matrix)
        {
            for (auto i : LoopRange<PointIndexType>(NumPoints)) {
                matrix(i, PlatformAxisIndex) = get_point_coord<PlatformAxisIndex>(c, i);
            }
        }
        
        template <typename TheCorrectionsMatrix>
        static void print_correction (Context c, TheCommand *cmd, TheCorrectionsMatrix const *corrections)
        {
            cmd->reply_append_ch(c, ' ');
            cmd->reply_append_ch(c, AxisName);
            cmd->reply_append_ch(c, ':');
            cmd->reply_append_fp(c, (*corrections)++(PlatformAxisIndex, 0));
        }
        
        template <typename Src, typename TheCorrectionsMatrix>
        static FpType calc_correction_contribution (FpType accum, Context c, Src src, TheCorrectionsMatrix const *corrections)
        {
            return accum + src.template get<VirtAxisIndex()>() * (*corrections)++(PlatformAxisIndex, 0);
        }
        
        struct Object : public ObjBase<AxisHelper, typename BedProbeModule::Object, EmptyTypeList> {};
    };
    using AxisHelperList = IndexElemList<PlatformAxesList, AxisHelper>;
    
    class ProbePlannerClient : public ThePrinterMain::PlannerClient {
    private:
        void pull_handler (Context c)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(o->m_current_point != -1)
            AMBRO_ASSERT(o->m_point_state <= 4)
            
            if (o->m_command_sent) {
                ThePrinterMain::custom_planner_wait_finished(c);
                return;
            }
            
            ThePrinterMain::move_begin(c);
            
            FpType height;
            FpType speed;
            switch (o->m_point_state) {
                case 0: {
                    ListFor<AxisHelperList>([&] APRINTER_TL(helper, helper::add_axis(c, o->m_current_point)));
                    height = APRINTER_CFG(Config, CProbeStartHeight, c);
                    speed = APRINTER_CFG(Config, CProbeMoveSpeed, c);
                } break;
                case 1: {
                    height = APRINTER_CFG(Config, CProbeLowHeight, c);
                    speed = APRINTER_CFG(Config, CProbeFastSpeed, c);
                } break;
                case 2: {
                    height = get_height(c) + APRINTER_CFG(Config, CProbeRetractDist, c);
                    speed = APRINTER_CFG(Config, CProbeRetractSpeed, c);
                } break;
                case 3: {
                    height = APRINTER_CFG(Config, CProbeLowHeight, c);
                    speed = APRINTER_CFG(Config, CProbeSlowSpeed, c);
                } break;
                case 4: {
                    height = o->m_single_point_mode ?
                        get_height(c) + o->m_single_point_retract_dist :
                        APRINTER_CFG(Config, CProbeStartHeight, c);
                    speed = APRINTER_CFG(Config, CProbeRetractSpeed, c);
                } break;
            }
            
            ThePrinterMain::template move_add_axis<ProbeAxisIndex>(c, height, true);
            ThePrinterMain::move_set_max_speed(c, speed);
            o->m_command_sent = true;
            return ThePrinterMain::move_end(c, ThePrinterMain::get_locked(c), BedProbeModule::move_end_callback);
        }
        
        void finished_handler (Context c, bool aborted)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(o->m_current_point != -1)
            AMBRO_ASSERT(o->m_point_state <= 4)
            AMBRO_ASSERT(o->m_command_sent)
            AMBRO_ASSERT(!aborted || is_point_state_watching(o->m_point_state))
            
            ThePrinterMain::custom_planner_deinit(c);
            o->m_command_sent = false;
            
            if (o->m_move_error) {
                return finish_probing(c, AMBRO_PSTR("Move"));
            }
            
            if (is_point_state_watching(o->m_point_state) && !aborted) {
                return finish_probing(c, AMBRO_PSTR("EndstopNotTriggeredInProbeMove"));
            }
            
            if (o->m_point_state == 4) {
                if (o->m_single_point_mode) {
                    o->m_current_point = -1;
                } else {
                    o->m_current_point++;
                    skip_disabled_points_and_detect_end(c);
                }
                if (o->m_current_point == -1) {
                    return finish_probing(c, nullptr);
                }
                init_probe_planner(c, false);
                o->m_point_state = 0;
                return;
            }
            
            if (o->m_point_state == 3) {
                if (!o->m_single_point_mode) {
                    FpType height = get_height(c) + APRINTER_CFG(Config, CProbeGeneralZOffset, c) + get_point_z_offset(c, o->m_current_point);
                    report_height(c, ThePrinterMain::get_locked(c), o->m_current_point, height);
                }
            }
            
            o->m_point_state++;
            bool watch_probe = is_point_state_watching(o->m_point_state);
            
            if (watch_probe && endstop_is_triggered(c)) {
                return finish_probing(c, AMBRO_PSTR("EndstopTriggeredBeforeProbeMove"));
            }
            
            init_probe_planner(c, watch_probe);
        }
    };
    
    static void init_probe_planner (Context c, bool watch_probe)
    {
        auto *o = Object::self(c);
        ThePrinterMain::custom_planner_init(c, &o->planner_client, watch_probe);
    }
    
    static FpType get_height (Context c)
    {
        return ThePrinterMain::template PhysVirtAxisHelper<ProbeAxisIndex>::get_position(c);
    }
    
    static void report_height (Context c, TheCommand *cmd, PointIndexType point_index, FpType height)
    {
        CorrectionFeature::probing_measurement(c, point_index, height);
        
        cmd->reply_append_pstr(c, AMBRO_PSTR("//ProbeHeight@P"));
        cmd->reply_append_uint32(c, point_index + 1);
        cmd->reply_append_ch(c, ' ');
        cmd->reply_append_fp(c, height);
        cmd->reply_append_ch(c, '\n');
        cmd->reply_poke(c, true);
    }
    
    static void finish_probing (Context c, AMBRO_PGM_P errstr)
    {
        auto *o = Object::self(c);
        
        bool run_hook = false;
        TheCommand *cmd = ThePrinterMain::get_locked(c);
        if (errstr) {
            cmd->reportError(c, errstr);
        }
        else if (!o->m_single_point_mode) {
            run_hook = CorrectionFeature::probing_completing(c, cmd);
        }
        
        if (!run_hook) {
            o->m_current_point = -1;
            return cmd->finishCommand(c);
        }
        
        o->m_current_point = -2;
        return ThePrinterMain::template startHookByInitiator<ServiceList::AfterBedProbingHookService>(c);
    }
    
    static void bed_probe_hook_completed (Context c, bool error)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->m_current_point == -2)
        
        TheCommand *cmd = ThePrinterMain::get_locked(c);
        if (error) {
            cmd->reportError(c, nullptr);
        }
        o->m_current_point = -1;
        cmd->finishCommand(c);
    }
    struct BedProbeHookCompletedHandler : public AMBRO_WFUNC_TD(&BedProbeModule::bed_probe_hook_completed) {};
    
    static bool is_point_state_watching (PointIndexType point_state)
    {
        return point_state == 1 || point_state == 3;
    }
    
    static void move_end_callback (Context c, bool error)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->m_current_point != -1)
        AMBRO_ASSERT(o->m_command_sent)
        
        if (error) {
            o->m_move_error = true;
        }
    }
    
    using CProbeInvert = decltype(ExprCast<bool>(Config::e(Params::ProbeInvert::i())));
    using CProbeStartHeight = decltype(ExprCast<FpType>(Config::e(Params::ProbeStartHeight::i())));
    using CProbeLowHeight = decltype(ExprCast<FpType>(Config::e(Params::ProbeLowHeight::i())));
    using CProbeRetractDist = decltype(ExprCast<FpType>(Config::e(Params::ProbeRetractDist::i())));
    using CProbeMoveSpeed = decltype(ExprCast<FpType>(Config::e(Params::ProbeMoveSpeed::i())));
    using CProbeFastSpeed = decltype(ExprCast<FpType>(Config::e(Params::ProbeFastSpeed::i())));
    using CProbeRetractSpeed = decltype(ExprCast<FpType>(Config::e(Params::ProbeRetractSpeed::i())));
    using CProbeSlowSpeed = decltype(ExprCast<FpType>(Config::e(Params::ProbeSlowSpeed::i())));
    using CProbeGeneralZOffset = decltype(ExprCast<FpType>(Config::e(Params::ProbeGeneralZOffset::i())));
    
public:
    using ConfigExprs = MakeTypeList<
        CProbeInvert, CProbeStartHeight, CProbeLowHeight, CProbeRetractDist, CProbeMoveSpeed,
        CProbeFastSpeed, CProbeRetractSpeed, CProbeSlowSpeed, CProbeGeneralZOffset
    >;
    
public:
    struct Object : public ObjBase<BedProbeModule, ParentObject, JoinTypeLists<
        PointHelperList,
        AxisHelperList,
        MakeTypeList<CorrectionFeature>
    >> {
        ProbePlannerClient planner_client;
        FpType m_single_point_retract_dist;
        PointIndexType m_current_point;
        bool m_single_point_mode;
        uint8_t m_point_state;
        bool m_command_sent;
        bool m_move_error;
    };
};

struct BedProbeNoCorrectionParams {
    static bool const Enabled = false;
};

APRINTER_ALIAS_STRUCT_EXT(BedProbeCorrectionParams, (
    APRINTER_AS_VALUE(bool, QuadraticCorrectionSupported),
    APRINTER_AS_TYPE(QuadraticCorrectionEnabled)
), (
    static bool const Enabled = true;
))

APRINTER_ALIAS_STRUCT(BedProbePointParams, (
    APRINTER_AS_TYPE(Enabled),
    APRINTER_AS_TYPE(Coords),
    APRINTER_AS_TYPE(ZOffset)
))

APRINTER_ALIAS_STRUCT_EXT(BedProbeModuleService, (
    APRINTER_AS_TYPE(PlatformAxesList),
    APRINTER_AS_VALUE(char, ProbeAxis),
    APRINTER_AS_TYPE(ProbePin),
    APRINTER_AS_TYPE(ProbePinInputMode),
    APRINTER_AS_TYPE(ProbeInvert),
    APRINTER_AS_TYPE(ProbePlatformOffset),
    APRINTER_AS_TYPE(ProbeStartHeight),
    APRINTER_AS_TYPE(ProbeLowHeight),
    APRINTER_AS_TYPE(ProbeRetractDist),
    APRINTER_AS_TYPE(ProbeMoveSpeed),
    APRINTER_AS_TYPE(ProbeFastSpeed),
    APRINTER_AS_TYPE(ProbeRetractSpeed),
    APRINTER_AS_TYPE(ProbeSlowSpeed),
    APRINTER_AS_TYPE(ProbeGeneralZOffset),
    APRINTER_AS_TYPE(ProbePoints),
    APRINTER_AS_TYPE(ProbeCorrectionParams)
), (
    APRINTER_MODULE_TEMPLATE(BedProbeModuleService, BedProbeModule)
    
    using ProvidedServices = If<ProbeCorrectionParams::Enabled, MakeTypeList<ServiceDefinition<ServiceList::CorrectionService>>, EmptyTypeList>;
))

}

#endif
