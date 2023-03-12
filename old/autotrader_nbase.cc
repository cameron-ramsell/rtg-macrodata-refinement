// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 10;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
	
	if(sequenceNumber <= mOrderBookSequence) {
		RLOG(LG_AT, LogLevel::LL_INFO) << "received old order book information.";
		return;
	}
	mOrderBookSequence = sequenceNumber;
	
    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    if (instrument != Instrument::FUTURE) {
        return;
    }

    unsigned long priceAdjustment = 100;
    unsigned long newAskPrice = (askPrices[0] != 0) ? askPrices[0] + priceAdjustment : 0;
    unsigned long newBidPrice = (bidPrices[0] != 0) ? bidPrices[0] - priceAdjustment : 0;

    if (mAskId != 0 && newAskPrice != 0 && newAskPrice != mAskPrice)
    {
        SendCancelOrder(mAskId);
        mAskId = 0;
    }
    if (mBidId != 0 && newBidPrice != 0 && newBidPrice != mBidPrice)
    {
        SendCancelOrder(mBidId);
        mBidId = 0;
    }

    if (mAskId == 0 && newAskPrice != 0 && (mETFPosition - mETFOrderPositionSell - LOT_SIZE) >= -POSITION_LIMIT)
    {
        mAskId = mNextMessageId++;
        mAskPrice = newAskPrice;
        SendInsertOrder(mAskId, Side::SELL, newAskPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);

        mETFOrderPositionSell += LOT_SIZE;
        mAsks[mAskId] = { mAskPrice, LOT_SIZE, 0 };
    }
    if (mBidId == 0 && newBidPrice != 0 && (mETFPosition + mETFOrderPositionBuy + LOT_SIZE) <= POSITION_LIMIT)
    {
        mBidId = mNextMessageId++;
        mBidPrice = newBidPrice;
        SendInsertOrder(mBidId, Side::BUY, newBidPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);

        mETFOrderPositionBuy += LOT_SIZE;
        mBids[mBidId] = { mBidPrice, LOT_SIZE, 0 };
    }

}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order filled message " << clientOrderId << " " << price << " " << volume;
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{

    RLOG(LG_AT, LogLevel::LL_INFO) << "order status message received "
        << clientOrderId << " " << fillVolume << " " << remainingVolume << " " << fees;

    bool isSellOrder = mAsks.count(clientOrderId);
    if(!isSellOrder && !mBids.count(clientOrderId)) {
        RLOG(LG_AT, LogLevel::LL_INFO) << "received order status for order we are not tracking. id=" << clientOrderId;
        return;
    }

    auto& sideMap = isSellOrder ? mAsks : mBids;
    Order& order = sideMap[clientOrderId];

    // Update our futures position to make sure we are correctly hedged
    auto dFilled = fillVolume - order.filledVolume;
    if(dFilled > 0) {
        mETFPosition += isSellOrder ? -dFilled : dFilled;
        SendHedgeOrder(mNextMessageId++,
                       isSellOrder ? Side::BUY : Side::SELL,
                       isSellOrder ? MAX_ASK_NEAREST_TICK : MIN_BID_NEARST_TICK, dFilled);
    }

    // Update the state
    auto dRemaining = order.remainingVolume - remainingVolume;
    if(isSellOrder) {
        mETFOrderPositionSell -= dRemaining;
    } else {
        mETFOrderPositionBuy -= dRemaining;
    }

    if(remainingVolume > 0) {
        order.remainingVolume = remainingVolume;
        order.filledVolume = fillVolume;
    } else {
        if(mBidId == clientOrderId) {
            mBidId = 0;
        }
        if(mAskId == clientOrderId) {
            mAskId = 0;
        }
        sideMap.erase(clientOrderId);
    }

}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];
}
