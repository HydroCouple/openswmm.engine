#pragma once
#include "core/CompletedSourceArchive.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include "hydrology/surface/MeshSnow.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef SWMM_USE_OPENMP
#include <omp.h>
#endif

namespace snow_held_test {
struct Case {
    openswmm::SimulationContext context;
    openswmm::surface::MeshSnow snow;
    std::vector<double> rain, catch_factor;
    int count, kind;
    Case(int n, int threads, int si, int scenario) : count(n), kind(scenario) {
        using namespace openswmm;
        context.options.num_threads = threads;
        context.options.flow_units = static_cast<decltype(context.options.flow_units)>(si ? 3 : 0);
        context.options.ignore_snow_melt = false;
        context.options.start_date = datetime::encodeDate(2026, 1, 1);
        context.options.end_date = context.options.start_date + 14;
        context.climate_state.temperature = context.climate_state.temperature_src = 22;
        context.climate_state.snow_divt = 32;
        context.climate_state.humidity = 75;
        for (int m=0;m<12;++m) {
            context.options.humidity[m] = 75;
            context.options.wind_speed[m] = 2;
        }
        // A live resolver table exercises the shared cursor on scalar preparation.
        Table weather;
        weather.id = "held_temperature";
        weather.type = TableType::TIMESERIES;
        for (int h=0;h<=24*14;++h) {
            weather.x.push_back(context.options.start_date + double(h)/24);
            const double f = 22. + h%7;
            weather.y.push_back(si ? (f-32)*5/9 : f);
        }
        const int table_index=context.tables.add(weather.id,weather.type);
        context.tables[table_index]=std::move(weather);
        context.climate_state.temp_ts_index = 0;
        context.heat_config.cloud.configured = true;
        context.heat_config.cloud.fraction = .5;
        context.snowpack_names.add("DD"); context.snowpack_names.add("UEB");
        context.snowpacks.names = {"DD","UEB"};
        context.snowpacks.pervious.assign(2,std::array<double,7>{.02,.06,32,.1,6,.6,0});
        context.snowpacks.config.model = surface::SnowKind::DegreeDay;
        context.snowpacks.config.packs[1].model = surface::SnowKind::Ueb;
        context.snowpacks.config.site = surface::ClimateSite{45,-120,-8,1000};
        context.snowpacks.mesh_config.step = 1800;
        context.snowpacks.mesh_config.defaults["*"].pack = "UEB";
        std::vector<surface::MeshSnowGeometry> geometry;
        for (int i=0;i<n;++i) {
            geometry.push_back({"region_"+std::to_string(i%5),50.+7*(i%13),double(i%4)*3,double(i%6)*45});
            if (kind==2 && i==n-1) context.snowpacks.mesh_config.cells[i].pack="DD";
            if (kind==8 && i==n-1) context.snowpacks.mesh_config.cells[i].pack="NONE";
        }
        check(snow.initialize(context,geometry));
        std::vector<unsigned char> contact(n,0);
        if (kind==3 && n) contact.back()=1;
        check(snow.setGroundContactMask(contact));
        rain.assign(n,1e-7); catch_factor.assign(n,1.2);
    }
    static void check(const std::string& error) {
        if (!error.empty()) throw std::runtime_error(error);
    }
    std::string advance(double seconds) {
        const double start=snow.completedEnd();
        std::string error;
#ifdef SWMM_USE_OPENMP
        if (kind==1) {
#pragma omp parallel num_threads(4)
            {
#pragma omp single
                error=snow.advance(context,start,start+seconds,rain,catch_factor);
            }
            return error;
        }
#endif
        return snow.advance(context,start,start+seconds,rain,catch_factor);
    }
    std::string snapshot(const std::string& error={}) const {
        using namespace openswmm;
        detail::CompletedSourceArchive a;
        auto checkpoint=snow.saveCheckpoint();
        auto identity=snow.configurationIdentity();
        double completed=snow.completedEnd(),last=snow.lastSeconds();
        auto liquid=snow.liquidRates(),exposed=snow.exposedFractions();
        auto& radiation=context.climate_state.radiation;
        auto short_origin=radiation.shortwave_source,long_origin=radiation.longwave_source;
        bool shared=radiation.shared_heat_forcing;
        std::string message=error;
        a(message,checkpoint,identity,completed,last,liquid,exposed);
        auto rad=radiation;
        a(rad.shortwave,rad.longwave,rad.cosine,rad.horizontal,rad.transmittance,rad.cloud,
          rad.temperature,rad.humidity,rad.wind,rad.pressure,rad.declination,rad.hour_angle,
          rad.latitude,rad.date,rad.seconds,short_origin,long_origin,shared);
        for (auto& table:context.tables.tables) {
            int index=table.cursor.index,direction=table.cursor.direction;
            a(index,direction);
        }
        return a.bytes();
    }
    std::string history() {
        using namespace openswmm;
        detail::CompletedSourceArchive a;
        std::string magic="UEB held consumption history v1"; a(magic);
        // First call must resolve/prepare; second and third can consume held shares.
        for (int stage=0;stage<12;++stage) {
            double seconds=stage==3 ? 1740 : 30;
            if (kind==9 && stage==0) seconds=1;
            if (kind==9 && stage==1) seconds=1e-9;
            if (kind==4 && stage==2 && count) rain.back()*=2;
            if (kind==5 && stage==2) {
                context.forcing.climate_temperature_mode=ForcingMode::OVERRIDE;
                context.forcing.climate_temperature_value=27;
                context.forcing.climate_temperature_persist=ForcingPersist::RESET;
            }
            if (kind==6 && stage==2 && count) {
                // Adversarial owner corruption reaches the worker's normal refusal.
                // Two distinct diagnostics verify selection of the lowest cell.
                const_cast<surface::MeshSnowLedger&>(snow.ledgers()[count>1?1:0]).initial+=1;
                const_cast<surface::MeshSnowLedger&>(snow.ledgers().back()).initial+=1;
            }
            if (kind==7 && stage==2 && count) rain.back()=-1;
            const auto before=snow.saveCheckpoint();
            const auto error=advance(seconds);
            const bool refusal=count && (kind==6 || kind==7) && stage==2;
            if (refusal) {
                if (error.empty() || snow.saveCheckpoint()!=before)
                    throw std::runtime_error("Expected refusal preserving the complete snow owner");
            } else check(error);
            auto frame=snapshot(error); a(stage,frame);
            if (refusal) {
                if (kind==7) rain.back()=1e-7;
                else {
                    const_cast<surface::MeshSnowLedger&>(snow.ledgers()[count>1?1:0]).initial-=1;
                    const_cast<surface::MeshSnowLedger&>(snow.ledgers().back()).initial-=1;
                }
            }
        }
        return a.bytes();
    }
};
inline void write(const std::filesystem::path& path,const std::string& bytes) {
    std::ofstream f(path,std::ios::binary);f.write(bytes.data(),std::streamsize(bytes.size()));
    f.close();if(!f.good())throw std::runtime_error("Cannot write UEB held evidence");
}
}
