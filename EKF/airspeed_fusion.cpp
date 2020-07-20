/****************************************************************************
 *
 *   Copyright (c) 2015 Estimation and Control Library (ECL). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name ECL nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file airspeed_fusion.cpp
 * airspeed fusion methods.
 * equations generated using EKF/python/ekf_derivation/main.py
 *
 * @author Carl Olsson <carlolsson.co@gmail.com>
 * @author Roman Bast <bapstroman@gmail.com>
 * @author Paul Riseborough <p_riseborough@live.com.au>
 *
 */
#include "../ecl.h"
#include "ekf.h"
#include <mathlib/mathlib.h>

void Ekf::fuseAirspeed()
{
	float Hfusion[5];  // Observation Jacobians - Note: indexing is different to state vector
	Vector24f Kfusion; // Kalman gain vector

	const float &vn = _state.vel(0); // Velocity in north direction
	const float &ve = _state.vel(1); // Velocity in east direction
	const float &vd = _state.vel(2); // Velocity in downwards direction
	const float &vwn = _state.wind_vel(0); // Wind speed in north direction
	const float &vwe = _state.wind_vel(1); // Wind speed in east direction

	// Variance for true airspeed measurement - (m/sec)^2
	const float R_TAS = sq(math::constrain(_params.eas_noise, 0.5f, 5.0f) *
			    math::constrain(_airspeed_sample_delayed.eas2tas, 0.9f, 10.0f));

	// determine if we need the sideslip fusion to correct states other than wind
	const bool update_wind_only = !_is_wind_dead_reckoning;

	// Intermediate variables
	const float HK0 = vn - vwn;
	const float HK1 = ve - vwe;
	const float HK2 = powf(HK0, 2) + powf(HK1, 2) + powf(vd, 2);
	if (HK2 < 1.0f) {
		// calculation can be badly conditioned for very low airspeed values so don't fuse this time
		return;
	}
	const float v_tas_pred = sqrtf(HK2); // predicted airspeed
	//const float HK3 = powf(HK2, -1.0F/2.0F);
	const float HK3 = 1.0f / v_tas_pred;
	const float HK4 = HK0*HK3;
	const float HK5 = HK1*HK3;
	const float HK6 = 1.0F/HK2;
	const float HK7 = HK0*P(4,6) - HK0*P(6,22) + HK1*P(5,6) - HK1*P(6,23) + P(6,6)*vd;
	const float HK8 = HK1*P(5,23);
	const float HK9 = HK0*P(4,5) - HK0*P(5,22) + HK1*P(5,5) - HK8 + P(5,6)*vd;
	const float HK10 = HK1*HK6;
	const float HK11 = HK0*P(4,22);
	const float HK12 = HK0*P(4,4) - HK1*P(4,23) + HK1*P(4,5) - HK11 + P(4,6)*vd;
	const float HK13 = HK0*HK6;
	const float HK14 = -HK0*P(22,23) + HK0*P(4,23) - HK1*P(23,23) + HK8 + P(6,23)*vd;
	const float HK15 = -HK0*P(22,22) - HK1*P(22,23) + HK1*P(5,22) + HK11 + P(6,22)*vd;
	float HK16;

	// innovation variance
	_airspeed_innov_var = (-HK10*HK14 + HK10*HK9 + HK12*HK13 - HK13*HK15 + HK6*HK7*vd + R_TAS);
	if (_airspeed_innov_var >= R_TAS) { // Check for badly conditioned calculation
		HK16 = HK3 / _airspeed_innov_var;
		_fault_status.flags.bad_airspeed = false;

	} else { // Reset the estimator covariance matrix
		_fault_status.flags.bad_airspeed = true;

		// if we are getting aiding from other sources, warn and reset the wind states and covariances only
		const char* action_string = nullptr;
		if (update_wind_only) {
			resetWindStates();
			resetWindCovariance();
			action_string = "wind";

		} else {
			initialiseCovariance();
			_state.wind_vel.setZero();
			action_string = "full";
		}
		ECL_ERR("airspeed badly conditioned - %s covariance reset", action_string);

		return;
	}

	// Observation Jacobians
	// Note: indexing is different to state vector 
	Hfusion[0] = HK4;    // corresponds to state index 4
	Hfusion[1] = HK5;    // corresponds to state index 5
	Hfusion[2] = HK3*vd; // corresponds to state index 6
	Hfusion[3] = -HK4;   // corresponds to state index 22
	Hfusion[4] = -HK5;   // corresponds to state index 23

	if (!update_wind_only) {
		// we have no other source of aiding, so use airspeed measurements to correct states
		for (unsigned row = 0; row <= 4; row++) {
			Kfusion(row) = HK16*(-HK0*P(row,22) + HK0*P(row,4) - HK1*P(row,23) + HK1*P(row,5) + P(row,6)*vd);
		}

		Kfusion(4) = HK12*HK16;
		Kfusion(5) = HK16*HK9;
		Kfusion(6) = HK16*HK7;

		for (unsigned row = 7; row <= 9; row++) {
			Kfusion(row) = HK16*(HK0*P(4,row) - HK0*P(row,22) + HK1*P(5,row) - HK1*P(row,23) + P(6,row)*vd);
		}

		for (unsigned row = 10; row <= 21; row++) {
			Kfusion(row) = HK16*(-HK0*P(row,22) + HK0*P(4,row) - HK1*P(row,23) + HK1*P(5,row) + P(6,row)*vd);
		}

	}
	Kfusion(22) = HK15*HK16;
	Kfusion(23) = HK14*HK16;


	// Calculate measurement innovation
	_airspeed_innov = v_tas_pred - _airspeed_sample_delayed.true_airspeed;

	// Compute the ratio of innovation to gate size
	_tas_test_ratio = sq(_airspeed_innov) / (sq(fmaxf(_params.tas_innov_gate, 1.0f)) * _airspeed_innov_var);

	// If the innovation consistency check fails then don't fuse the sample and indicate bad airspeed health
	if (_tas_test_ratio > 1.0f) {
		_innov_check_fail_status.flags.reject_airspeed = true;
		return;

	} else {
		_innov_check_fail_status.flags.reject_airspeed = false;
	}

	// Airspeed measurement sample has passed check so record it
	_time_last_arsp_fuse = _time_last_imu;

	// apply covariance correction via P_new = (I -K*H)*P
	// first calculate expression for KHP
	// then calculate P - KHP
	matrix::SquareMatrix<float, _k_num_states> KHP;
	float KH[5];

	for (unsigned row = 0; row < _k_num_states; row++) {

		for (unsigned index = 0; index < 5; index++) {
			KH[index] = Kfusion(row) * Hfusion[index];
		}

		for (unsigned column = 0; column < _k_num_states; column++) {
			float tmp = KH[0] * P(4,column);
			tmp += KH[1] * P(5,column);
			tmp += KH[2] * P(6,column);
			tmp += KH[3] * P(22,column);
			tmp += KH[4] * P(23,column);
			KHP(row,column) = tmp;
		}
	}

	const bool healthy = checkAndFixCovarianceUpdate(KHP);

	_fault_status.flags.bad_airspeed = !healthy;

	if (healthy) {
		// apply the covariance corrections
		P -= KHP;

		fixCovarianceErrors(true);

		// apply the state corrections
		fuse(Kfusion, _airspeed_innov);

		_time_last_arsp_fuse = _time_last_imu;

	}
}

Vector2f Ekf::getWindVelocity() const
{
	return _state.wind_vel;
}

Vector2f Ekf::getWindVelocityVariance() const
{
	return P.slice<2, 2>(22,22).diag();
}

void Ekf::get_true_airspeed(float *tas)
{
	float tempvar = sqrtf(sq(_state.vel(0) - _state.wind_vel(0)) + sq(_state.vel(1) - _state.wind_vel(1)) + sq(_state.vel(2)));
	memcpy(tas, &tempvar, sizeof(float));
}

/*
 * Reset the wind states using the current airspeed measurement, ground relative nav velocity, yaw angle and assumption of zero sideslip
*/
void Ekf::resetWindStates()
{
	const Eulerf euler321(_state.quat_nominal);
	const float euler_yaw = euler321(2);

	if (_tas_data_ready && (_imu_sample_delayed.time_us - _airspeed_sample_delayed.time_us < (uint64_t)5e5)) {
		// estimate wind using zero sideslip assumption and airspeed measurement if airspeed available
		_state.wind_vel(0) = _state.vel(0) - _airspeed_sample_delayed.true_airspeed * cosf(euler_yaw);
		_state.wind_vel(1) = _state.vel(1) - _airspeed_sample_delayed.true_airspeed * sinf(euler_yaw);

	} else {
		// If we don't have an airspeed measurement, then assume the wind is zero
		_state.wind_vel.setZero();
	}
}
