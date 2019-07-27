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

#ifndef APRINTER_AUX_CONTROL_MODULE_H
#define APRINTER_AUX_CONTROL_MODULE_H

#include <stdint.h>
#include <math.h>

#include <aprinter/meta/FuncUtils.h>
#include <aprinter/meta/ListForEach.h>
#include <aprinter/meta/TypeListUtils.h>
#include <aprinter/meta/PowerOfTwo.h>
#include <aprinter/meta/Union.h>
#include <aprinter/meta/UnionGet.h>
#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/MemberType.h>
#include <aprinter/meta/BasicMetaUtils.h>
#include <aprinter/meta/StructIf.h>
#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/Callback.h>
#include <aprinter/base/ProgramMemory.h>
#include <aprinter/base/Lock.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Hints.h>
#include <aprinter/system/InterruptLock.h>
#include <aprinter/printer/Configuration.h>
#include <aprinter/printer/planning/MotionPlanner.h>
#include <aprinter/misc/ClockUtils.h>
#include <aprinter/printer/utils/JsonBuilder.h>
#include <aprinter/printer/utils/ModuleUtils.h>

namespace APrinter {

template <typename ModuleArg>
class AuxControlModule {
    APRINTER_UNPACK_MODULE_ARG(ModuleArg)
    
public:
    struct Object;
    
public:
    using ReservedHeaterFanNames = MakeTypeList<WrapInt<'F'>, WrapInt<'S'>>;
    
private:
    using Clock = typename Context::Clock;
    using TimeType = typename Clock::TimeType;
    using TheClockUtils = ClockUtils<Context>;
    using Config = typename ThePrinterMain::Config;
    using TheOutputStream = typename ThePrinterMain::TheOutputStream;
    using TheCommand = typename ThePrinterMain::TheCommand;
    using FpType = typename ThePrinterMain::FpType;
    using TimeConversion = typename ThePrinterMain::TimeConversion;
    using PhysVirtAxisMaskType = typename ThePrinterMain::PhysVirtAxisMaskType;
    
    using ParamsHeatersList = typename Params::HeatersList;
    using ParamsFansList = typename Params::FansList;
    static int const NumHeaters = TypeListLength<ParamsHeatersList>::Value;
    static int const NumFans = TypeListLength<ParamsFansList>::Value;
    
    using CWaitTimeoutTicks = decltype(ExprCast<TimeType>(Config::e(Params::WaitTimeout::i()) * TimeConversion()));
    using CWaitReportPeriodTicks = decltype(ExprCast<TimeType>(Config::e(Params::WaitReportPeriod::i()) * TimeConversion()));
    
    static int const SetHeaterCommand = 104;
    static int const PrintHeatersCommand = 105;
    static int const SetFanCommand = 106;
    static int const OffFanCommand = 107;
    static int const SetWaitHeaterCommand = 109;
    static int const WaitHeatersCommand = 116;
    static int const PrintAdcCommand = 921;
    static int const ClearErrorCommand = 922;
    static int const ColdExtrudeCommand = 302;
    
    AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_ChannelPayload, ChannelPayload)
    
public:
    static void init (Context c)
    {
        auto *o = Object::self(c);
        o->waiting_heaters = 0;
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::init(c)));
        ListFor<FansList>([&] APRINTER_TL(fan, fan::init(c)));
    }
    
    static void deinit (Context c)
    {
        ListForReverse<FansList>([&] APRINTER_TL(fan, fan::deinit(c)));
        ListForReverse<HeatersList>([&] APRINTER_TL(heater, heater::deinit(c)));
    }
    
    static bool check_command (Context c, TheCommand *cmd)
    {
        if (cmd->getCmdNumber(c) == SetHeaterCommand) {
            handle_set_heater_command(c, cmd, /*wait=*/false);
            return false;
        }
        if (cmd->getCmdNumber(c) == SetWaitHeaterCommand) {
            handle_set_heater_command(c, cmd, /*wait=*/true);
            return false;
        }
        if (cmd->getCmdNumber(c) == PrintHeatersCommand) {
            handle_print_heaters_command(c, cmd);
            return false;
        }
        if (cmd->getCmdNumber(c) == SetFanCommand || cmd->getCmdNumber(c) == OffFanCommand) {
            handle_set_fan_command(c, cmd, cmd->getCmdNumber(c) == OffFanCommand);
            return false;
        }
        if (cmd->getCmdNumber(c) == WaitHeatersCommand) {
            handle_wait_heaters_command(c, cmd);
            return false;
        }
        if (cmd->getCmdNumber(c) == PrintAdcCommand) {
            handle_print_adc_command(c, cmd);
            return false;
        }
        if (cmd->getCmdNumber(c) == ClearErrorCommand) {
            handle_clear_error_command(c, cmd);
            return false;
        }
        if (cmd->getCmdNumber(c) == ColdExtrudeCommand) {
            handle_cold_extrude_command(c, cmd);
            return false;
        }
        return ListForBreak<HeatersList>([&] APRINTER_TL(heater, return heater::check_command(c, cmd))) &&
               ListForBreak<FansList>([&] APRINTER_TL(fan, return fan::check_command(c, cmd)));
    }
    
    static void emergency ()
    {
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::emergency()));
        ListFor<FansList>([&] APRINTER_TL(fan, fan::emergency()));
    }
    
    static void check_safety (Context c)
    {
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::check_safety(c)));
    }
    
    static bool check_move_interlocks (Context c, TheOutputStream *err_output, PhysVirtAxisMaskType move_axes)
    {
        return ListForBreak<HeatersList>([&] APRINTER_TL(heater, return heater::check_move_interlocks(c, err_output, move_axes)));
    }
    
    template <typename TheJsonBuilder>
    static void get_json_status (Context c, TheJsonBuilder *json)
    {
        if (NumHeaters > 0) {
            json->addKeyObject(JsonSafeString{"heaters"});
            ListFor<HeatersList>([&] APRINTER_TL(heater, heater::get_json_status(c, json)));
            json->endObject();
        }
        
        if (NumFans > 0) {
            json->addKeyObject(JsonSafeString{"fans"});
            ListFor<FansList>([&] APRINTER_TL(fan, fan::get_json_status(c, json)));
            json->endObject();
        }
    }
    
private:
    template <typename Name>
    static void print_name (Context c, TheOutputStream *cmd)
    {
        cmd->reply_append_ch(c, Name::Letter);
        if (Name::Number != 0) {
            cmd->reply_append_uint32(c, Name::Number);
        }
    }
    
    template <typename Name, typename TheJsonBuilder>
    static void print_json_name (Context c, TheJsonBuilder *json)
    {
        if (Name::Number == 0) {
            json->add(JsonSafeChar{Name::Letter});
        } else {
            char str[3] = {Name::Letter, '0' + Name::Number, '\0'};
            json->add(JsonSafeString{str});
        }
    }
    
    template <typename Name>
    static bool match_name (Context c, TheCommand *cmd)
    {
        typename TheCommand::PartRef part;
        return cmd->find_command_param(c, Name::Letter, &part) && cmd->getPartUint32Value(c, part) == Name::Number;
    }
    
    struct HeaterState {
        FpType current;
        FpType target;
        bool error;
    };
    
    template <int HeaterIndex>
    struct Heater {
        struct Object;
        struct ObserverGetValueCallback;
        struct ObserverHandler;
        
        using HeaterSpec = TypeListGet<ParamsHeatersList, HeaterIndex>;
        static_assert(NameCharIsValid<HeaterSpec::Name::Letter, ReservedHeaterFanNames>::Value, "Heater name not allowed");
        
        using ControlInterval = decltype(Config::e(HeaterSpec::ControlInterval::i()));
        APRINTER_MAKE_INSTANCE(TheControl, (HeaterSpec::ControlService::template Control<Context, Object, Config, ControlInterval, FpType>))
        APRINTER_MAKE_INSTANCE(ThePwm, (HeaterSpec::PwmService::template Pwm<Context, Object>))
        APRINTER_MAKE_INSTANCE(TheObserver, (HeaterSpec::ObserverService::template Observer<Context, Object, Config, FpType, ObserverGetValueCallback, ObserverHandler>))
        using PwmDutyCycleData = typename ThePwm::DutyCycleData;
        APRINTER_MAKE_INSTANCE(TheFormula, (HeaterSpec::Formula::template Formula<Context, Object, Config, FpType>))
        APRINTER_MAKE_INSTANCE(TheAnalogInput, (HeaterSpec::AnalogInput::template AnalogInput<Context, Object>))
        using AdcFixedType = typename TheAnalogInput::FixedType;
        using AdcIntType = typename AdcFixedType::IntType;
        using MinSafeTemp = decltype(Config::e(HeaterSpec::MinSafeTemp::i()));
        using MaxSafeTemp = decltype(Config::e(HeaterSpec::MaxSafeTemp::i()));
        
        // compute the ADC readings corresponding to MinSafeTemp and MaxSafeTemp
        using AdcRange = APRINTER_FP_CONST_EXPR((PowerOfTwo<double, AdcFixedType::num_bits>::Value));
        template <typename Temp>
        static auto TempToAdcAbs (Temp) -> decltype(TheFormula::TempToAdc(Temp()) * AdcRange());
        using AdcFpLowLimit = APRINTER_FP_CONST_EXPR(1.0 + 0.1);
        using AdcFpHighLimit = APRINTER_FP_CONST_EXPR((PowerOfTwoMinusOne<AdcIntType, AdcFixedType::num_bits>::Value - 0.1));
        using InfAdcSafeTemp = If<TheFormula::NegativeSlope, decltype(MaxSafeTemp()), decltype(MinSafeTemp())>;
        using SupAdcSafeTemp = If<TheFormula::NegativeSlope, decltype(MinSafeTemp()), decltype(MaxSafeTemp())>;
        using InfAdcValueFp = decltype(ExprFmax(AdcFpLowLimit(), TempToAdcAbs(InfAdcSafeTemp())));
        using SupAdcValueFp = decltype(ExprFmin(AdcFpHighLimit(), TempToAdcAbs(SupAdcSafeTemp())));
        
        using CMinSafeTemp = decltype(ExprCast<FpType>(MinSafeTemp()));
        using CMaxSafeTemp = decltype(ExprCast<FpType>(MaxSafeTemp()));
        using CInfAdcValue = decltype(ExprCast<AdcIntType>(InfAdcValueFp()));
        using CSupAdcValue = decltype(ExprCast<AdcIntType>(SupAdcValueFp()));
        using CControlIntervalTicks = decltype(ExprCast<TimeType>(ControlInterval() * TimeConversion()));
        
        struct ChannelPayload {
            FpType target;
        };
        
        template <typename This=AuxControlModule>
        static constexpr typename This::HeatersMaskType HeaterMask () { return (HeatersMaskType)1 << HeaterIndex; }
        
        static void init (Context c)
        {
            auto *o = Object::self(c);
            o->m_enabled = false;
            o->m_was_not_unset = false;
            o->m_report_thermal_runaway = false;
            o->m_target = NAN;
            TimeType time = Clock::getTime(c) + (TimeType)(0.05 * TimeConversion::value());
            o->m_control_event.init(c, APRINTER_CB_STATFUNC_T(&Heater::control_event_handler));
            o->m_control_event.appendAt(c, time + (APRINTER_CFG(Config, CControlIntervalTicks, c) / 2));
            ThePwm::init(c, time);
            TheObserver::init(c);
            TheAnalogInput::init(c);
            ColdExtrusionFeature::init(c);
        }
        
        static void deinit (Context c)
        {
            auto *o = Object::self(c);
            TheAnalogInput::deinit(c);
            TheObserver::deinit(c);
            ThePwm::deinit(c);
            o->m_control_event.deinit(c);
        }
        
        static FpType adc_to_temp (Context c, AdcFixedType adc_value)
        {
            if (TheAnalogInput::isValueInvalid(adc_value)) {
                return NAN;
            }
            FpType adc_fp = adc_value.template fpValue<FpType>();
            if (!TheAnalogInput::IsRounded) {
                adc_fp += (FpType)(0.5 / PowerOfTwo<double, AdcFixedType::num_bits>::Value);
            }
            return TheFormula::adcToTemp(c, adc_fp);
        }
        
        static AdcFixedType get_adc (Context c)
        {
            return TheAnalogInput::getValue(c);
        }
        
        static bool adc_is_unsafe (Context c, AdcFixedType adc_value)
        {
            return
                TheAnalogInput::isValueInvalid(adc_value) ||
                adc_value.bitsValue() <= APRINTER_CFG(Config, CInfAdcValue, c) ||
                adc_value.bitsValue() >= APRINTER_CFG(Config, CSupAdcValue, c);
        }
        
        static FpType get_temp (Context c)
        {
            AdcFixedType adc_raw = get_adc(c);
            return adc_to_temp(c, adc_raw);
        }
        struct ObserverGetValueCallback : public AMBRO_WFUNC_TD(&Heater::get_temp) {};
        
        static HeaterState get_state (Context c)
        {
            auto *o = Object::self(c);
            
            HeaterState st;
            st.current = get_temp(c);
            bool enabled;
            AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                st.target = o->m_target;
                enabled = o->m_enabled;
            }
            st.error = (!FloatIsNan(st.target) && !enabled);
            return st;
        }
        
        static void append_value (Context c, TheOutputStream *cmd)
        {
            HeaterState st = get_state(c);
            
            cmd->reply_append_ch(c, ' ');
            print_name<typename HeaterSpec::Name>(c, cmd);
            cmd->reply_append_ch(c, ':');
            cmd->reply_append_fp(c, st.current);
            cmd->reply_append_pstr(c, AMBRO_PSTR(" /"));
            cmd->reply_append_fp(c, st.target);
            if (st.error) {
                cmd->reply_append_pstr(c, AMBRO_PSTR(",err"));
            }
        }
        
        static void append_adc_value (Context c, TheCommand *cmd)
        {
            AdcFixedType adc_value = get_adc(c);
            cmd->reply_append_ch(c, ' ');
            print_name<typename HeaterSpec::Name>(c, cmd);
            cmd->reply_append_pstr(c, AMBRO_PSTR("A:"));
            cmd->reply_append_fp(c, adc_value.template fpValue<FpType>());
        }
        
        static void clear_error (Context c, TheCommand *cmd)
        {
            auto *o = Object::self(c);
            
            FpType target;
            bool enabled;
            AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                target = o->m_target;
                enabled = o->m_enabled;
            }
            
            if (!FloatIsNan(target) && !enabled) {
                set(c, target);
            }
        }
        
        template <typename ThisContext>
        static void set (ThisContext c, FpType target)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(!FloatIsNan(target))
            
            AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                o->m_target = target;
                o->m_enabled = true;
            }
        }
        
        template <typename ThisContext>
        static void unset (ThisContext c, bool orderly)
        {
            auto *o = Object::self(c);
            AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                if (orderly) {
                    o->m_target = NAN;
                } else if (o->m_enabled) {
                    o->m_report_thermal_runaway = true;
                }
                o->m_enabled = false;
                o->m_was_not_unset = false;
                PwmDutyCycleData duty;
                ThePwm::computeZeroDutyCycle(&duty);
                ThePwm::setDutyCycle(lock_c, duty);
            }
        }
        
        template <typename ThisContext>
        static void set_or_unset (ThisContext c, FpType target)
        {
            if (AMBRO_LIKELY(!FloatIsNan(target))) {
                set(c, target);
            } else {
                unset(c, true);
            }
        }
        
        static bool check_set_command (Context c, TheCommand *cmd, bool wait, bool force, bool use_default)
        {
            bool is_own_command = wait ?
                (HeaterSpec::SetWaitMCommand == SetWaitHeaterCommand) :
                (HeaterSpec::SetMCommand == SetHeaterCommand);
            
            if (!use_default ? match_name<typename HeaterSpec::Name>(c, cmd) : is_own_command) {
                handle_set_command(c, cmd, wait, force);
                return false;
            }
            return true;
        }
        
        static bool check_command (Context c, TheCommand *cmd)
        {
            if (HeaterSpec::SetMCommand != 0 && HeaterSpec::SetMCommand != SetHeaterCommand && cmd->getCmdNumber(c) == HeaterSpec::SetMCommand) {
                bool force = cmd->find_command_param(c, 'F', nullptr);
                if (force || cmd->tryPlannedCommand(c)) {
                    handle_set_command(c, cmd, /*wait=*/false, force);
                }
                return false;
            }
            if (HeaterSpec::SetWaitMCommand != 0 && HeaterSpec::SetWaitMCommand != SetWaitHeaterCommand && cmd->getCmdNumber(c) == HeaterSpec::SetWaitMCommand) {
                if (cmd->tryUnplannedCommand(c)) {
                    handle_set_command(c, cmd, /*wait=*/true, /*force=*/false);
                }
                return false;
            }
            return true;
        }
        
        static void handle_set_command (Context c, TheCommand *cmd, bool wait, bool force)
        {
            FpType target = cmd->get_command_param_fp(c, 'S', 0.0f);
            if (!(target >= APRINTER_CFG(Config, CMinSafeTemp, c) && target <= APRINTER_CFG(Config, CMaxSafeTemp, c))) {
                target = NAN;
            }

            if (!wait) {
                cmd->finishCommand(c);
            }

            if (force || wait) {
                set_or_unset(c, target);
            } else {
                auto *planner_cmd = ThePlanner<>::getBuffer(c);
                PlannerChannelPayload *payload = UnionGetElem<PlannerChannelIndex<>::Value>(&planner_cmd->channel_payload);
                payload->type = HeaterIndex;
                UnionGetElem<HeaterIndex>(&payload->heaters)->target = target;
                ThePlanner<>::channelCommandDone(c, PlannerChannelIndex<>::Value + 1);
                ThePrinterMain::submitted_planner_command(c);
            }

            if (wait) {
                do_wait_heaters(c, cmd, HeaterMask());
            }
        }
        
        static void check_safety (Context c)
        {
            TheAnalogInput::check_safety(c);
            
            AdcFixedType adc_value = get_adc(c);
            if (adc_is_unsafe(c, adc_value)) {
                unset(c, false);
            }
        }
        
        static void observer_handler (Context c, bool state)
        {
            auto *o = Object::self(c);
            auto *mo = AuxControlModule::Object::self(c);
            AMBRO_ASSERT(TheObserver::isObserving(c))
            AMBRO_ASSERT(mo->waiting_heaters & HeaterMask())
            
            if (state) {
                mo->inrange_heaters |= HeaterMask();
            } else {
                mo->inrange_heaters &= ~HeaterMask();
            }
            check_wait_completion(c);
        }
        struct ObserverHandler : public AMBRO_WFUNC_TD(&Heater::observer_handler) {};
        
        static void emergency ()
        {
            ThePwm::emergency();
        }
        
        template <typename ThisContext, typename TheChannelPayloadUnion>
        static void channel_callback (ThisContext c, TheChannelPayloadUnion *payload_union)
        {
            ChannelPayload *payload = UnionGetElem<HeaterIndex>(payload_union);
            set_or_unset(c, payload->target);
        }
        
        static void control_event_handler (Context c)
        {
            auto *o = Object::self(c);
            
            o->m_control_event.appendAfterPrevious(c, APRINTER_CFG(Config, CControlIntervalTicks, c));
            
            AdcFixedType adc_value = get_adc(c);
            if (adc_is_unsafe(c, adc_value)) {
                unset(c, false);
            }
            
            bool enabled;
            FpType target;
            bool was_not_unset;
            bool report_thermal_runaway;
            AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                enabled = o->m_enabled;
                target = o->m_target;
                was_not_unset = o->m_was_not_unset;
                o->m_was_not_unset = enabled;
                report_thermal_runaway = o->m_report_thermal_runaway;
                o->m_report_thermal_runaway = false;
            }
            if (AMBRO_LIKELY(enabled)) {
                if (!was_not_unset) {
                    TheControl::init(c);
                }
                FpType sensor_value = adc_to_temp(c, adc_value);
                if (!FloatIsNan(sensor_value)) {
                    FpType output = TheControl::addMeasurement(c, sensor_value, target);
                    PwmDutyCycleData duty;
                    ThePwm::computeDutyCycle(output, &duty);
                    AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                        if (o->m_was_not_unset) {
                            ThePwm::setDutyCycle(lock_c, duty);
                        }
                    }
                }
            }
            
            if (report_thermal_runaway) {
                auto *output = ThePrinterMain::get_msg_output(c);
                output->reply_append_pstr(c, AMBRO_PSTR("//"));
                print_heater_error(c, output, AMBRO_PSTR("HeaterThermalRunaway"));
                output->reply_poke(c);
            }
            
            if (TheObserver::isObserving(c) && !enabled) {
                TheCommand *cmd = ThePrinterMain::get_locked(c);
                print_heater_error(c, cmd, AMBRO_PSTR("HeaterThermalRunaway"));
                complete_wait(c, true, nullptr);
            }
            
            maybe_report(c);
        }
        
        template <typename TheHeatersMaskType>
        static void update_wait_mask (Context c, TheCommand *cmd, TheHeatersMaskType *mask)
        {
            if (match_name<typename HeaterSpec::Name>(c, cmd)) {
                *mask |= HeaterMask();
            }
        }
        
        template <typename TheHeatersMaskType>
        static bool start_wait (Context c, TheCommand *cmd, TheHeatersMaskType mask)
        {
            auto *o = Object::self(c);
            auto *mo = AuxControlModule::Object::self(c);
            
            if ((mask & HeaterMask()) || mask == 0) {
                FpType target;
                bool enabled;
                AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                    target = o->m_target;
                    enabled = o->m_enabled;
                }
                
                if (!FloatIsNan(target)) {
                    if (!enabled) {
                        print_heater_error(c, cmd, AMBRO_PSTR("HeaterThermalRunaway"));
                        return false;
                    }
                    mo->waiting_heaters |= HeaterMask();
                    TheObserver::startObserving(c, target);
                }
                else if ((mask & HeaterMask())) {
                    print_heater_error(c, cmd, AMBRO_PSTR("HeaterNotEnabled"));
                    return false;
                }
            }
            
            return true;
        }
        
        static void print_heater_error (Context c, TheOutputStream *cmd, AMBRO_PGM_P errstr)
        {
            cmd->reply_append_pstr(c, AMBRO_PSTR("Error:"));
            cmd->reply_append_pstr(c, errstr);
            cmd->reply_append_ch(c, ':');
            print_name<typename HeaterSpec::Name>(c, cmd);
            cmd->reply_append_ch(c, '\n');
        }
        
        static void stop_wait (Context c)
        {
            auto *mo = AuxControlModule::Object::self(c);
            
            if ((mo->waiting_heaters & HeaterMask())) {
                TheObserver::stopObserving(c);
            }
        }
        
        static bool check_move_interlocks (Context c, TheOutputStream *err_output, PhysVirtAxisMaskType move_axes)
        {
            return ColdExtrusionFeature::check_move_interlocks(c, err_output, move_axes);
        }
        
        template <typename TheJsonBuilder>
        static void get_json_status (Context c, TheJsonBuilder *json)
        {
            HeaterState st = get_state(c);
            
            print_json_name<typename HeaterSpec::Name>(c, json);
            json->entryValue();
            json->startObject();
            json->addSafeKeyVal("current", JsonDouble{st.current});
            json->addSafeKeyVal("target", JsonDouble{st.target});
            json->addSafeKeyVal("error", JsonBool{st.error});
            json->endObject();
        }
        
        static void print_cold_extrude (Context c, TheOutputStream *output)
        {
            ColdExtrusionFeature::print_cold_extrude(c, output);
        }
        
        template <typename TheHeatersMaskType>
        static void set_cold_extrude (Context c, bool allow, TheHeatersMaskType heaters_mask)
        {
            ColdExtrusionFeature::set_cold_extrude(c, allow, heaters_mask);
        }
        
        AMBRO_STRUCT_IF(ColdExtrusionFeature, HeaterSpec::ColdExtrusion::Enabled) {
            template <typename AxisName, typename AccumMask>
            using ExtrudersMaskFoldFunc = WrapValue<PhysVirtAxisMaskType, (AccumMask::Value | ThePrinterMain::template GetPhysVirtAxisByName<AxisName::Value>::AxisMask)>;
            using ExtrudersMask = TypeListFold<typename HeaterSpec::ColdExtrusion::ExtruderAxes, WrapValue<PhysVirtAxisMaskType, 0>, ExtrudersMaskFoldFunc>;
            
            using CMinExtrusionTemp = decltype(ExprCast<FpType>(Config::e(HeaterSpec::ColdExtrusion::MinExtrusionTemp::i())));
            using ConfigExprs = MakeTypeList<CMinExtrusionTemp>;
            
            static void init (Context c)
            {
                auto *o = Object::self(c);
                o->cold_extrusion_allowed = false;
            }
            
            static bool check_move_interlocks (Context c, TheOutputStream *err_output, PhysVirtAxisMaskType move_axes)
            {
                auto *o = Object::self(c);
                if (!o->cold_extrusion_allowed && (move_axes & ExtrudersMask::Value)) {
                    FpType temp = get_temp(c);
                    if (!(temp >= APRINTER_CFG(Config, CMinExtrusionTemp, c)) || isinf(temp)) {
                        err_output->reply_append_pstr(c, AMBRO_PSTR("Error:"));
                        err_output->reply_append_pstr(c, AMBRO_PSTR("ColdExtrusionPrevented:"));
                        print_name<typename HeaterSpec::Name>(c, err_output);
                        err_output->reply_append_ch(c, '\n');
                        return false;
                    }
                }
                return true;
            }
            
            static void print_cold_extrude (Context c, TheOutputStream *output)
            {
                auto *o = Object::self(c);
                output->reply_append_ch(c, ' ');
                print_name<typename HeaterSpec::Name>(c, output);
                output->reply_append_ch(c, '=');
                output->reply_append_ch(c, o->cold_extrusion_allowed ? '1' : '0');
            }
            
            template <typename TheHeatersMaskType>
            static void set_cold_extrude (Context c, bool allow, TheHeatersMaskType heaters_mask)
            {
                auto *o = Object::self(c);
                if ((heaters_mask & HeaterMask())) {
                    o->cold_extrusion_allowed = allow;
                }
            }
            
            struct Object : public ObjBase<ColdExtrusionFeature, typename Heater::Object, EmptyTypeList> {
                bool cold_extrusion_allowed;
            };
        }
        AMBRO_STRUCT_ELSE(ColdExtrusionFeature) {
            static void init (Context c) {}
            static bool check_move_interlocks (Context c, TheOutputStream *err_output, PhysVirtAxisMaskType move_axes) { return true; }
            static void print_cold_extrude (Context c, TheOutputStream *output) {}
            template <typename TheHeatersMaskType>
            static void set_cold_extrude (Context c, bool allow, TheHeatersMaskType heaters_mask) {}
            struct Object {};
        };
        
        struct Object : public ObjBase<Heater, typename AuxControlModule::Object, MakeTypeList<
            TheControl,
            ThePwm,
            TheObserver,
            TheFormula,
            TheAnalogInput,
            ColdExtrusionFeature
        >> {
            uint8_t m_enabled : 1;
            uint8_t m_was_not_unset : 1;
            uint8_t m_report_thermal_runaway : 1;
            FpType m_target;
            typename Context::EventLoop::TimedEvent m_control_event;
        };
        
        using ConfigExprs = MakeTypeList<CMinSafeTemp, CMaxSafeTemp, CInfAdcValue, CSupAdcValue, CControlIntervalTicks>;
    };
    
    template <int FanIndex>
    struct Fan {
        struct Object;
        
        using FanSpec = TypeListGet<ParamsFansList, FanIndex>;
        static_assert(NameCharIsValid<FanSpec::Name::Letter, ReservedHeaterFanNames>::Value, "Fan name not allowed");
        
        APRINTER_MAKE_INSTANCE(ThePwm, (FanSpec::PwmService::template Pwm<Context, Object>))
        using PwmDutyCycleData = typename ThePwm::DutyCycleData;
        
        struct ChannelPayload {
            PwmDutyCycleData duty;
        };
        
        static void init (Context c)
        {
            TimeType time = Clock::getTime(c) + (TimeType)(0.05 * TimeConversion::value());
            ThePwm::init(c, time);
        }
        
        static void deinit (Context c)
        {
            ThePwm::deinit(c);
        }
        
        static bool check_set_command (Context c, TheCommand *cmd, bool force, bool is_turn_off, bool use_default)
        {
            if (!use_default ? match_name<typename FanSpec::Name>(c, cmd) : (
                !is_turn_off ?
                (FanSpec::SetMCommand != 0 && SetFanCommand == FanSpec::SetMCommand) :
                (FanSpec::OffMCommand != 0 && OffFanCommand == FanSpec::OffMCommand)
            )) {
                handle_set_command(c, cmd, force, is_turn_off);
                return false;
            }
            return true;
        }
        
        static bool check_command (Context c, TheCommand *cmd)
        {
            if ((FanSpec::SetMCommand != 0 && FanSpec::SetMCommand != SetFanCommand && cmd->getCmdNumber(c) == FanSpec::SetMCommand) ||
                (FanSpec::OffMCommand != 0 && FanSpec::OffMCommand != OffFanCommand && cmd->getCmdNumber(c) == FanSpec::OffMCommand)
            ) {
                bool force = cmd->find_command_param(c, 'F', nullptr);
                if (force || cmd->tryPlannedCommand(c)) {
                    bool is_turn_off = (cmd->getCmdNumber(c) == FanSpec::OffMCommand);
                    handle_set_command(c, cmd, force, is_turn_off);
                }
                return false;
            }
            return true;
        }
        
        static void handle_set_command (Context c, TheCommand *cmd, bool force, bool is_turn_off)
        {
            FpType target = 0.0f;
            if (!is_turn_off) {
                target = 1.0f;
                if (cmd->find_command_param_fp(c, 'S', &target)) {
                    target *= (FpType)FanSpec::SpeedMultiply::value();
                }
            }
            
            cmd->finishCommand(c);
            
            PwmDutyCycleData duty;
            ThePwm::computeDutyCycle(target, &duty);
            
            if (force) {
                ThePwm::setDutyCycle(c, duty);
            } else {
                auto *planner_cmd = ThePlanner<>::getBuffer(c);
                PlannerChannelPayload *payload = UnionGetElem<PlannerChannelIndex<>::Value>(&planner_cmd->channel_payload);
                payload->type = NumHeaters + FanIndex;
                UnionGetElem<FanIndex>(&payload->fans)->duty = duty;
                ThePlanner<>::channelCommandDone(c, PlannerChannelIndex<>::Value + 1);
                ThePrinterMain::submitted_planner_command(c);
            }
        }
        
        template <typename TheJsonBuilder>
        static void get_json_status (Context c, TheJsonBuilder *json)
        {
            FpType target = ThePwm::template getCurrentDutyFp<FpType>(c);
            
            print_json_name<typename FanSpec::Name>(c, json);
            json->entryValue();
            json->startObject();
            json->addSafeKeyVal("target", JsonDouble{target});
            json->endObject();
        }
        
        static void emergency ()
        {
            ThePwm::emergency();
        }
        
        template <typename ThisContext, typename TheChannelPayloadUnion>
        static void channel_callback (ThisContext c, TheChannelPayloadUnion *payload_union)
        {
            ChannelPayload *payload = UnionGetElem<FanIndex>(payload_union);
            ThePwm::setDutyCycle(c, payload->duty);
        }
        
        struct Object : public ObjBase<Fan, typename AuxControlModule::Object, MakeTypeList<
            ThePwm
        >> {};
    };
    
    using HeatersList = IndexElemList<ParamsHeatersList, Heater>;
    using FansList = IndexElemList<ParamsFansList, Fan>;
    
    using HeatersChannelPayloadUnion = Union<MapTypeList<HeatersList, GetMemberType_ChannelPayload>>;
    using FansChannelPayloadUnion = Union<MapTypeList<FansList, GetMemberType_ChannelPayload>>;
    
    using HeatersMaskType = ChooseInt<MaxValue(1, NumHeaters), false>;
    static HeatersMaskType const AllHeatersMask = PowerOfTwoMinusOne<HeatersMaskType, NumHeaters>::Value;
    
    struct PlannerChannelPayload {
        uint8_t type;
        union {
            HeatersChannelPayloadUnion heaters;
            FansChannelPayloadUnion fans;
        };
    };
    
    template <typename This=AuxControlModule> struct PlannerChannelCallback;
    struct PlannerChannelSpec : public MotionPlannerChannelSpec<PlannerChannelPayload, PlannerChannelCallback<AuxControlModule>, Params::EventChannelBufferSize, typename Params::EventChannelTimerService> {};
    
    template <typename This=AuxControlModule>
    using ThePlanner = typename This::ThePrinterMain::ThePlanner;
    
    template <typename This=AuxControlModule>
    using PlannerChannelIndex = typename This::ThePrinterMain::template GetPlannerChannelIndex<PlannerChannelSpec>;
    
    static void handle_set_heater_command (Context c, TheCommand *cmd, bool wait)
    {
        bool force = !wait && cmd->find_command_param(c, 'F', nullptr);
        if (wait ? !cmd->tryUnplannedCommand(c) : (!force && !cmd->tryPlannedCommand(c))) {
            return;
        }
        if (ListForBreak<HeatersList>([&] APRINTER_TL(heater, return heater::check_set_command(c, cmd, wait, force, false))) &&
            ListForBreak<HeatersList>([&] APRINTER_TL(heater, return heater::check_set_command(c, cmd, wait, force, true)))
        ) {
            if (NumHeaters > 0) {
                cmd->reportError(c, AMBRO_PSTR("UnknownHeater"));
            }
            cmd->finishCommand(c);
        }
    }
    
    static void handle_print_heaters_command (Context c, TheCommand *cmd)
    {
        cmd->reply_append_pstr(c, AMBRO_PSTR("ok"));
        print_heaters(c, cmd);
        cmd->finishCommand(c, true);
    }
    
    static void print_heaters (Context c, TheOutputStream *cmd)
    {
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::append_value(c, cmd)));
        cmd->reply_append_ch(c, '\n');
    }
    
    static void handle_set_fan_command (Context c, TheCommand *cmd, bool is_turn_off)
    {
        bool force = cmd->find_command_param(c, 'F', nullptr);
        if (!force && !cmd->tryPlannedCommand(c)) {
            return;
        }
        if (ListForBreak<FansList>([&] APRINTER_TL(fan, return fan::check_set_command(c, cmd, force, is_turn_off, false))) &&
            ListForBreak<FansList>([&] APRINTER_TL(fan, return fan::check_set_command(c, cmd, force, is_turn_off, true)))
        ) {
            if (NumFans > 0) {
                cmd->reportError(c, AMBRO_PSTR("UnknownFan"));
            }
            cmd->finishCommand(c);
        }
    }
    
    static void handle_wait_heaters_command (Context c, TheCommand *cmd)
    {
        if (!cmd->tryUnplannedCommand(c)) {
            return;
        }
        HeatersMaskType heaters_mask = 0;
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::update_wait_mask(c, cmd, &heaters_mask)));
        do_wait_heaters(c, cmd, heaters_mask);
    }

    static void do_wait_heaters (Context c, TheCommand *cmd, HeatersMaskType heaters_mask)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->waiting_heaters == 0)
        o->waiting_heaters = 0;
        o->inrange_heaters = 0;
        o->wait_started_time = Clock::getTime(c);
        if (!ListForBreak<HeatersList>([&] APRINTER_TL(heater, return heater::start_wait(c, cmd, heaters_mask)))) {
            cmd->reportError(c, nullptr);
            cmd->finishCommand(c);
            ListFor<HeatersList>([&] APRINTER_TL(heater, heater::stop_wait(c)));
            o->waiting_heaters = 0;
            return;
        }
        if (o->waiting_heaters) {
            o->report_poll_timer.setTo(o->wait_started_time);
            ThePrinterMain::now_active(c);
        } else {
            cmd->finishCommand(c);
        }
    }
    
    static void handle_print_adc_command (Context c, TheCommand *cmd)
    {
        cmd->reply_append_pstr(c, AMBRO_PSTR("ok"));
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::append_adc_value(c, cmd)));
        cmd->reply_append_ch(c, '\n');
        cmd->finishCommand(c, true);
    }
    
    static void handle_clear_error_command (Context c, TheCommand *cmd)
    {
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::clear_error(c, cmd)));
        cmd->finishCommand(c);
    }
    
    static void handle_cold_extrude_command (Context c, TheCommand *cmd)
    {
        if (!cmd->find_command_param(c, 'P', nullptr)) {
            cmd->reply_append_pstr(c, AMBRO_PSTR("ColdExtrude:"));
            ListFor<HeatersList>([&] APRINTER_TL(heater, heater::print_cold_extrude(c, cmd)));
            cmd->reply_append_ch(c, '\n');
        } else {
            bool allow = (cmd->get_command_param_uint32(c, 'P', 0) > 0);
            HeatersMaskType heaters_mask = 0;
            ListFor<HeatersList>([&] APRINTER_TL(heater, heater::update_wait_mask(c, cmd, &heaters_mask)));
            if (heaters_mask == 0) {
                heaters_mask = AllHeatersMask;
            }
            ListFor<HeatersList>([&] APRINTER_TL(heater, heater::set_cold_extrude(c, allow, heaters_mask)));
        }
        cmd->finishCommand(c);
    }
    
    static void complete_wait (Context c, bool error, AMBRO_PGM_P errstr)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->waiting_heaters)
        
        TheCommand *cmd = ThePrinterMain::get_locked(c);
        if (error) {
            cmd->reportError(c, errstr);
        }
        cmd->finishCommand(c);
        ListFor<HeatersList>([&] APRINTER_TL(heater, heater::stop_wait(c)));
        o->waiting_heaters = 0;
        ThePrinterMain::now_inactive(c);
    }
    
    static void check_wait_completion (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->waiting_heaters)
        
        bool reached = o->inrange_heaters == o->waiting_heaters;
        bool timed_out = (TimeType)(Clock::getTime(c) - o->wait_started_time) >= APRINTER_CFG(Config, CWaitTimeoutTicks, c);
        
        if (reached || timed_out) {
            complete_wait(c, timed_out, AMBRO_PSTR("WaitTimedOut"));
        }
    }
    
    static void maybe_report (Context c)
    {
        auto *o = Object::self(c);
        if (o->waiting_heaters && o->report_poll_timer.isExpired(c)) {
            o->report_poll_timer.addTime(APRINTER_CFG(Config, CWaitReportPeriodTicks, c));
            auto *output = ThePrinterMain::get_msg_output(c);
            output->reply_append_pstr(c, AMBRO_PSTR("//HeatProgress"));
            print_heaters(c, output);
            output->reply_poke(c);
        }
    }
    
    template <typename This>
    static void planner_channel_callback (typename ThePlanner<This>::template Channel<PlannerChannelIndex<This>::Value>::CallbackContext c, PlannerChannelPayload *payload)
    {
        auto *ob = Object::self(c);
        
        ListForOneBool<HeatersList, 0>(payload->type, [&] APRINTER_TL(heater, heater::channel_callback(c, &payload->heaters))) ||
        ListForOneBool<FansList, NumHeaters>(payload->type, [&] APRINTER_TL(fan, fan::channel_callback(c, &payload->fans)));
    }
    template <typename This> struct PlannerChannelCallback : public AMBRO_WFUNC_TD(&AuxControlModule::template planner_channel_callback<This>) {};
    
public:
    template <typename This=AuxControlModule>
    using GetEventChannelTimer = typename ThePlanner<This>::template GetChannelTimer<PlannerChannelIndex<This>::Value>;
    
    template <int HeaterIndex>
    using GetHeaterPwm = typename Heater<HeaterIndex>::ThePwm;
    
    template <int HeaterIndex>
    using GetHeaterAnalogInput = typename Heater<HeaterIndex>::TheAnalogInput;
    
    template <int FanIndex>
    using GetFanPwm = typename Fan<FanIndex>::ThePwm;
    
    using MotionPlannerChannels = MakeTypeList<PlannerChannelSpec>;
    
    using ConfigExprs = MakeTypeList<CWaitTimeoutTicks, CWaitReportPeriodTicks>;
    
public:
    struct Object : public ObjBase<AuxControlModule, ParentObject, JoinTypeLists<
        HeatersList,
        FansList
    >> {
        HeatersMaskType waiting_heaters;
        HeatersMaskType inrange_heaters;
        TimeType wait_started_time;
        typename TheClockUtils::PollTimer report_poll_timer;
    };
};

APRINTER_ALIAS_STRUCT(AuxControlName, (
    APRINTER_AS_VALUE(char, Letter),
    APRINTER_AS_VALUE(uint8_t, Number)
))

struct AuxControlNoColdExtrusionParams {
    static bool const Enabled = false;
};

APRINTER_ALIAS_STRUCT_EXT(AuxControlColdExtrusionParams, (
    APRINTER_AS_TYPE(MinExtrusionTemp),
    APRINTER_AS_TYPE(ExtruderAxes)
), (
    static bool const Enabled = true;
))

APRINTER_ALIAS_STRUCT(AuxControlModuleHeaterParams, (
    APRINTER_AS_TYPE(Name),
    APRINTER_AS_VALUE(int, SetMCommand),
    APRINTER_AS_VALUE(int, SetWaitMCommand),
    APRINTER_AS_TYPE(AnalogInput),
    APRINTER_AS_TYPE(Formula),
    APRINTER_AS_TYPE(MinSafeTemp),
    APRINTER_AS_TYPE(MaxSafeTemp),
    APRINTER_AS_TYPE(ControlInterval),
    APRINTER_AS_TYPE(ControlService),
    APRINTER_AS_TYPE(ObserverService),
    APRINTER_AS_TYPE(PwmService),
    APRINTER_AS_TYPE(ColdExtrusion)
))

APRINTER_ALIAS_STRUCT(AuxControlModuleFanParams, (
    APRINTER_AS_TYPE(Name),
    APRINTER_AS_VALUE(int, SetMCommand),
    APRINTER_AS_VALUE(int, OffMCommand),
    APRINTER_AS_TYPE(SpeedMultiply),
    APRINTER_AS_TYPE(PwmService)
))

APRINTER_ALIAS_STRUCT_EXT(AuxControlModuleService, (
    APRINTER_AS_VALUE(int, EventChannelBufferSize),
    APRINTER_AS_TYPE(EventChannelTimerService),
    APRINTER_AS_TYPE(WaitTimeout),
    APRINTER_AS_TYPE(WaitReportPeriod),
    APRINTER_AS_TYPE(HeatersList),
    APRINTER_AS_TYPE(FansList)
), (
    APRINTER_MODULE_TEMPLATE(AuxControlModuleService, AuxControlModule)
))

}

#endif
