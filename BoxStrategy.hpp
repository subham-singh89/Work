#pragma once

#include <optional>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <stack>
#include <functional>
#include <mutex>

#include "log4cpp/LoggerWrapper.hpp"
#include "gmClientApi.hpp"
#include "gmCodes.hpp"
#include "json.hpp"
#include "gmConfigurationMaster.hpp"
#include "SQLAPI.h"


#define GM_BOX_PRICE_MULTIPLIER     100UL       // Price multipler upto 2 decimal places
#define MAX_BOX                     100UL       // Maximum number of box objects
#define MAX_ORDERINFO               800UL       // Maximum number of orderinfo objects
#define MAX_LEG_IN_BOX              4UL         // Maxizmum number of legs in a box

// TODO: remove tickCount after testing,
// It is added to have a log line that show spread and contract's price
// when placing any box order.
uint16_t tickCount=0;

// Stratgety logger
gmLogger *logger = nullptr;

struct ConfigPaths
{
    std::string loggerConfig;
};

struct Contracts
{
    int lowerCallId, lowerPutId, higherCallId, higherPutId;
};

struct BoxStrategyConfig
{
    std::string groupIp;
    std::string interfaceIp;
    int groupPort;
    double strike_diff;
    int32_t Qty;
    double requiredBuyEntrySpread;
    double requiredSellEntrySpread;
    std::string symbol;
    std::string expiry;
    int strike_price;
    int client_id;
    int dealer_id;
    ConfigPaths configs;
    Contracts contracts_id;
    int strategyId;
    int noBoxOrderAfterHour;
};

namespace gm::boxstrategy
{
    using ContractId = int;
    using Spread = int;
    using Qty = uint;
    using Price = int;

    // Box type, buy or sell box
    enum gmBoxType { BT_BUY = 0, BT_SELL = 1 };

    // Box state determine that all its leg order successfully placed or not
    enum gmBoxState { BS_COMPLETE = 0, BS_INCOMPLETE = 1};

    /**
     * Class to handle object pool.
     * 
     * It has pool of N objects created at constructor.
     * 
     * Anyone interested to use the object use getObject fucntion to get an acces to an object.
     * And to return the object use returnObject function.
     * 
     * Note: No boundary check while requesting to get an object
     * and Object are not reset to default value after return.
     * 
     */
    template <typename T>
    class ObjectPool
    {
        public:

            /**
             * Create an object pool with specified size.
             * 
             * @param size Size of the pool.
             */
            ObjectPool(uint size)
            {
                for(uint i = 0; i < size; ++i)
                {
                    objects.push(new T);
                }
            }

            ~ObjectPool()
            {
                std::lock_guard<std::mutex> guard(poolMutex);
                for(uint i = 0; i < objects.size(); ++i)
                {
                    delete objects.top();
                    objects.pop();
                }
            }

            /**
             * Get the object from the pool.
             * 
             * @return Valid pointer to object.
             * 
             * Note: No check on boundary, for faster access.
             */
            T* getObject()
            {
                std::lock_guard<std::mutex> guard(poolMutex);
                T * obj = objects.top();
                objects.pop();
                return obj;
            }

            /**
             * Return the object to the pool.
             * 
             * @param id Object's id to return to the pool.
             */
            void returnObject(T *obj)
            {
                std::lock_guard<std::mutex> guard(poolMutex);
                objects.push(obj);
            }

        private:
            std::stack<T*> objects;
            std::mutex poolMutex;
    };

    /**
     * Class to thread safe task queue.
     * 
     * Task has been push into queue and removed by pop functions.
     * Thread will take a task fro the queue using pop function and execute it.
     * 
     */
    template <typename T>
    class ThreadSafeQueue
    {
    public:
        void push(T value)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push(std::move(value));
            }
            condition_.notify_one();
        }

        bool pop(T &value)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]
                            { return !queue_.empty(); });
            value = std::move(queue_.front());
            queue_.pop();
            return true;
        }

    private:
        std::queue<T> queue_;
        std::mutex mutex_;
        std::condition_variable condition_;
    };

    // currently all of these values are fixed and passed during init
    struct BoxStrategyParams
    {
        ContractId lowerCallId_, lowerPutId_, higherCallId_, higherPutId_;
        Qty requiredBuyEntryQty_ = 1, requiredSellEntryQty_ = 1;
        Price strikeDiff_, strikePrice, targetPnL_;

        // Set to default values
        Spread requiredBuyEntrySpread_ = 0.5 * GM_BOX_PRICE_MULTIPLIER, requiredSellEntrySpread_ = 0.5 * GM_BOX_PRICE_MULTIPLIER;

        // Time in 24hours format after which no new box order are placed
        int noBoxOrderAfterHour;

        // following are not used in strategy logic
        int32_t strategyId_, clientId_, dealerId_;

        std::string toString() const
        {
            std::stringstream oss;
            oss << lowerCallId_ << " " << lowerPutId_ << " " << higherCallId_ << " " << higherPutId_ << " " << requiredBuyEntryQty_ << " " << requiredSellEntryQty_ << " " << requiredBuyEntryQty_ << " " << requiredSellEntryQty_ << " " << strategyId_ << " " << clientId_ << " " << dealerId_ << " " << strikeDiff_;
            return oss.str();
        }

        void setBoxStrategyParams(BoxStrategyConfig &strategyInitValues)
        {
            strikeDiff_ = strategyInitValues.strike_diff * GM_BOX_PRICE_MULTIPLIER;
            strikePrice = strategyInitValues.strike_price;
            strategyId_ = strategyInitValues.strategyId;
            clientId_ = strategyInitValues.client_id;
            dealerId_ = strategyInitValues.dealer_id;

            lowerCallId_ = strategyInitValues.contracts_id.lowerCallId;
            lowerPutId_ = strategyInitValues.contracts_id.lowerPutId;
            higherCallId_ = strategyInitValues.contracts_id.higherCallId;
            higherPutId_ = strategyInitValues.contracts_id.higherPutId;

            requiredBuyEntryQty_ = strategyInitValues.Qty;
            requiredSellEntryQty_ = strategyInitValues.Qty;
            requiredBuyEntrySpread_ = strategyInitValues.requiredBuyEntrySpread * GM_BOX_PRICE_MULTIPLIER;
            requiredSellEntrySpread_ = strategyInitValues.requiredSellEntrySpread * GM_BOX_PRICE_MULTIPLIER;

            noBoxOrderAfterHour = strategyInitValues.noBoxOrderAfterHour;
        }
    };

    // used to store last bid/ask for a contract. Can be extended to store full order
    // We can add this part in infra
    struct BookCache
    {
        // Price is an array of 3 side's price, where price[1] = buy price and price[2] = sell price.
        // This is done for faster accessing price as per side and price[0] is unused.
        Price price[3] = {0};
    };

    struct OrderInfo; // Forward declaration to use in Contract
    struct Contract
    {
        Contract(ContractId cid) : id(cid) {}
        ContractId id;
        BookCache book;
        std::unordered_map<int /*linkId*/, OrderInfo*> orders;
    };

    struct Box;     // Forward declaration to use in OrderInfo

    /**
     * OrderInfo
     * 
     * Represent an order in box strategy.
     */
    struct OrderInfo
    {
        int orderId = 0;
        int linkId = 0;
        u_int8_t legId = 0;    // Leg index to which the order belongs in a box order
        Qty qty = 0;
        Qty fillQty = 0;
        Price price = 0;
        gmBuyOrSell side = gmBuyOrSell::BS_NOT_SET;
        gmOrderStatus orderStatus = gmOrderStatus::OS_SENT_NEW;
        Contract *contract = nullptr;     // Contract associated to order. Never delete this contract pointer
        Box *box = nullptr;               // Box associated to order. Never delete this box pointer
    };

    /**
     * Box
     * 
     * Represent a box in box strategy.
     */
    struct Box
    {
        uint8_t boxId = 0;
        uint8_t legFillCount = 0;
        gmBoxType boxType = gmBoxType::BT_BUY;
        gmBoxState state = BS_COMPLETE;
        OrderInfo* legs[MAX_LEG_IN_BOX];
    };

    /**
     * Order
     * 
     * Order that need to be place
     */
    struct Order
    {
        Contract *contract;
        const Price price;
        const gmBuyOrSell side;
        const Qty qty = 0;
    };

    /**
     * BoxOrder
     * 
     * BoxOrder that need to be place
     */
    struct BoxOrder
    {
        const gmBoxType boxType;
        Spread boxSpread;

        const Order leg1;
        const Order leg2;
        const Order leg3;
        const Order leg4;
    };

    /**
     * Position
     * 
     * Holds Previous position of box stratgety 
     */
    struct Position
    {
        int32_t algoId;
        int32_t clientId;
        int32_t dealerId;
        bool isCall;
        Qty buyQty;
        Qty sellQty;
        std::string tradeDate;
        Price price;
        ContractId contractId;
    };

    /**
     * Class BoxStrategy
     * 
     * BoxStratgy class place box orders when conditions are met for the box.
     * It manages its all leg order and take care of any incomplete box in the system.
     */
    class BoxStrategy : public gmClientApi
    {
        public:
            BoxStrategy(const BoxStrategyParams &params, const std::string &loggerConfig, const uint64_t modifyIntervalInMicroSeconds) :
                                gmClientApi(loggerConfig), boxStrategyParams(params),
                                lowerCallContract(params.lowerCallId_), lowerPutContract(params.lowerPutId_),
                                higherCallContract(params.higherCallId_), higherPutContract(params.higherPutId_),
                                strategyStopped(false), noBoxOrder(false), dayClosed(false),
                                hasUnFilledBuyBox(false), hasUnFilledSellBox(false), contractOrderInfoMap(nullptr),
                                orderInfoObjects(MAX_ORDERINFO), boxObjects(MAX_BOX), modifyinterval(modifyIntervalInMicroSeconds)
            {
                // Create contracts map collection using contract's id
                contractIdToContract.emplace(lowerCallContract.id, &lowerCallContract);
                contractIdToContract.emplace(lowerPutContract.id, &lowerPutContract);
                contractIdToContract.emplace(higherCallContract.id, &higherCallContract);
                contractIdToContract.emplace(higherPutContract.id, &higherPutContract);

                // Get previous position if any, and create boxes and order info from them.
                // This to support intraday position and same day restarting of strategy.
                createBoxesFromPreviousPositions();

                // Starts all threads to do their work
                controlActivityThread = std::thread(&BoxStrategy::controlActivity, this);
                removeSquareOffBoxThread = std::thread(&BoxStrategy::removeSquareOffBox, this);
                removeIncompleteBoxThread = std::thread(&BoxStrategy::removeIncompleteBox, this);
                cancelSquareoffOrderThread = std::thread(&BoxStrategy::runCancelSquareoffOrderTask, this);
                modifyOrderThread = std::thread(&BoxStrategy::modifyOrder, this);
                boxOrderThread = std::thread(&BoxStrategy::runBoxOrderTask, this);
            }

        void subscribeSymbols()
        {
            // TODO confirm if this is the right way to subscribe
            std::vector<std::string> securityIds{
                std::to_string(boxStrategyParams.lowerCallId_),
                std::to_string(boxStrategyParams.lowerPutId_),
                std::to_string(boxStrategyParams.higherCallId_),
                std::to_string(boxStrategyParams.higherPutId_)};

            ::logger->writeLog(gmLogLevel::INFO, "Subscribing to all 4 symbols: ");

            for (const auto &securityId : securityIds)
            {
                std::cout << securityId << std::endl;
            }

            gmClientApi::subscribeSecurities(securityIds, securityIds);
        }

        /**
         * Not required for now.
         */
        void gmIndexBcCallback(const gmIndex &indexBc)
        {
        }

       void gmQuoteResponseCallback(const gmQuoteResponse &quoteResponse)
        {
        }

        /**
         * Get called when received order update from exchange.
         * 
         * @param orderUpdate Order update details
         */
        void gmOrderUpdateCallback(const gmOrderUpdate &orderUpdate) override
        {
            // TODO: remove after testing
            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Received orderUpdate for order id: %d, Contract id: %d, LinkId: %d, status: %d, request status: %d", orderUpdate.orderId, orderUpdate.contractId, orderUpdate.linkId, orderUpdate.orderStatus, orderUpdate.requestStatus));

            std::lock_guard<std::mutex> guard(orderInfoMutex);
            switch(orderUpdate.requestStatus)
            {
                case gmOrderStatus::OS_ACCEPTED:
                {
                    // Update the map to use orderId as key instead of linkId once order is accepted.
                    // After the order is accepted, we will receive orderId, which will then be used for all order tracking.
                    std::unordered_map<int, OrderInfo *>::iterator orderInfoItr = orderIdToOrderInfo.find(orderUpdate.linkId);
                    if (orderInfoItr == orderIdToOrderInfo.end())
                    {
                        // Either not our order or order already removed from the collection, due to fill
                        ::logger->writeLog(gmLogLevel::FATAL, gmStringHelper::formattedString("Received Accepted orderUpdate for order id: %d, Contract id: %d, LinkId: %d, and no such order exists.", orderUpdate.orderId, orderUpdate.contractId, orderUpdate.linkId));
                        return;
                    }

                    // Get orderinfo object and update its orderid, status and in orderinfo collection against its orderid
                    OrderInfo *orderInfo = orderInfoItr->second;
                    orderInfo->orderId = orderUpdate.orderId;
                    orderInfo->orderStatus = orderUpdate.orderStatus;
                    orderIdToOrderInfo.emplace(orderUpdate.orderId, orderInfo);

                    // Remove orderinfo object against its linkId, which is not required now.
                    orderIdToOrderInfo.erase(orderInfoItr);

                    // Order is a part of a incomplete box
                    if (orderInfo->box && gmBoxState::BS_INCOMPLETE == orderInfo->box->state)
                    {
                        // TODO: remove log line after testing
                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Order Accepted and its a part of incomplete box, need to cancel it, orderid: %d, Linkid: %d, boxid: %d, contractId: %d", orderUpdate.orderId, orderInfo->linkId, orderInfo->box->boxId, orderInfo->contract->id));

                        // We should cancel such order asap as it belongs to incomplete box.
                        // If it get skipped from this check, then it will be handled by incomplete thread
                        cancelSquareoffOrderTaskQueue.push([orderId = orderUpdate.orderId, this]()
                                                           { placeCancelOrder(orderId); });
                    }
                }
                break;
                case gmOrderStatus::OS_NEW_REJECTED:
                {
                    // Order is rejected, need to remove the order from the system.
                    // System still identified the order by its link id
                    std::unordered_map<int, OrderInfo *>::iterator orderInfoItr = orderIdToOrderInfo.find(orderUpdate.linkId);
                    if (orderInfoItr == orderIdToOrderInfo.end())
                    {
                        // Either not our order or order already removed from the collection, due to fill
                        ::logger->writeLog(gmLogLevel::FATAL, gmStringHelper::formattedString("Received rejected orderUpdate for order id: %d, Contract id: %d, LinkId: %d, and no such order exists.", orderUpdate.orderId, orderUpdate.contractId, orderUpdate.linkId));
                        return;
                    }

                    // Get order info object of the rejected order
                    OrderInfo *orderInfo = orderInfoItr->second;

                    {
                        // Remove order info from the contract
                        std::lock_guard<std::mutex> contractGaurd(contractMutex);
                        orderInfo->contract->orders.erase(orderInfo->linkId);
                    }

                    // Order is part of box and got rejected, marks the box incomplete
                    if (orderInfo->box)
                    {
                        // Set box to incomplete
                        orderInfo->box->state = gmBoxState::BS_INCOMPLETE;

                        // let make the order leg to null as removing orderinfo for it.
                        orderInfo->box->legs[orderInfo->legId] = nullptr;

                        // Add incomplete box to incomplete collection, so it will be handled quickly
                        std::lock_guard<std::mutex> incompleteBoxGaurd(incompleteBoxesMutex);
                        incompleteBoxes.emplace(orderInfo->box);
                        incompleteBoxAddedCond.notify_one();
                        ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("An Order rejected for a box, made box incomplete, order id: %d, linkid: %d, Contract id: %d, box: %d, Leg: %d", orderUpdate.orderId, orderInfo->linkId, orderUpdate.contractId, orderInfo->box->boxId, orderInfo->legId));
                    }

                    // Remove orderInfo object from orderInfo collection,
                    // and return the object to the object pool for reuse.
                    orderIdToOrderInfo.erase(orderInfoItr);
                    orderInfoObjects.returnObject(orderInfo);

                    ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("Order Rejected, order Id: %d, linkId: %d, contract: %d", orderUpdate.orderId, orderInfo->linkId, orderUpdate.contractId));
                }
                break;
                default:
                {
                    // Get orderInfo against orderId
                    std::unordered_map<int, OrderInfo *>::iterator orderInfoItr = orderIdToOrderInfo.find(orderUpdate.orderId);
                    if (orderInfoItr == orderIdToOrderInfo.end())
                    {
                        // Not our order, we shouldn't be here
                        ::logger->writeLog(gmLogLevel::FATAL, gmStringHelper::formattedString("Received orderUpdate for order id: %d, Contract id: %d, linkId: %d, and no such order exists.", orderUpdate.orderId, orderUpdate.contractId, orderUpdate.linkId));
                        return;
                    }

                    // Get order info object
                    OrderInfo *orderInfo = orderInfoItr->second;

                    switch(orderUpdate.requestStatus)
                    {
                        case gmOrderStatus::OS_CANCELLED:
                        {
                            // Cancelled order is part of box
                            if (orderInfo->box)
                            {
                                // let make the order leg to null as removing orderinfo for it.
                                orderInfo->box->legs[orderInfo->legId] = nullptr;
                            }

                            {
                                // Remove order info from the contract
                                std::lock_guard<std::mutex> contractGaurd(contractMutex);
                                orderInfo->contract->orders.erase(orderInfo->linkId);
                            }

                            // Remove orderInfo object from orderInfo collection that is cancelled,
                            // and return the object to the object pool for reuse.
                            orderIdToOrderInfo.erase(orderInfoItr);
                            orderInfoObjects.returnObject(orderInfo);

                            ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("Order Cancelled, order Id: %d, linkId: %d, contract: %d", orderUpdate.orderId, orderInfo->linkId, orderUpdate.contractId));
                        }
                        break;
                        default:
                        {
                            // Update its order status
                            orderInfo->orderStatus = orderUpdate.orderStatus;

                            // keep price in sync with what at the exchange.
                            // This handles the case of modify accpeted or rejected.
                            // Dividing by 100, as orderUpdate has 4 decimal place adjustment,
                            // whereas exchange has two decimal place adjustment, so keep them in sync.
                            orderInfo->price = orderUpdate.price / GM_BOX_PRICE_MULTIPLIER;
                        }
                        break;
                    }
                } // default
                break;
            }  // switch(orderUpdate.requestStatus)
        }

        /**
         * Get called when received order fill from exchange.
         * 
         * @param orderFill Order fill details
         */
        void gmOrderFillCallback(const gmOrderFill &orderFill) override
        {
             // TODO: remove after testing
            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Received orderFill for order id: %d, Contract id: %d", orderFill.orderId, orderFill.contractId));

            std::lock_guard<std::mutex> guard(orderInfoMutex);

            // Get orderInfo object against the orderId received in order fill
            std::unordered_map<int, OrderInfo *>::const_iterator orderInfoItr = orderIdToOrderInfo.find(orderFill.orderId);
            if (orderInfoItr == orderIdToOrderInfo.end())
            {
                // Not our order, we shouldn't be here
                ::logger->writeLog(gmLogLevel::FATAL, gmStringHelper::formattedString("Received orderFill for order id: %d, Contract id: %d, and no such order exists.", orderFill.orderId, orderFill.contractId));
                return;
            }

            // Get orderInfo object
            OrderInfo *orderInfo = orderInfoItr->second;

            // Update order status and fill quantity
            orderInfo->orderStatus = orderFill.orderStatus;
            orderInfo->fillQty = orderFill.fillQuantity;

            switch(orderFill.orderStatus)
            {
                case gmOrderStatus::OS_COMPLETE_FILL:
                {
                    // Is an order is a part of a box
                    if (orderInfo->box)
                    {
                        // Get the associated box with the order
                        Box *box = orderInfo->box;

                        // Got fill for a complete box?
                        if (gmBoxState::BS_COMPLETE == box->state)
                        {
                            // Are all legs are filled for a box?
                            if (MAX_LEG_IN_BOX == ++box->legFillCount)
                            {
                                // TODO: remove this log line after testing
                                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("All 4 legs filled for box: %d, need to check for box squareoff", box->boxId));

                                // Strategy now has no unfilled box of a particular box type
                                std::lock_guard<std::mutex> gaurdFilledBox(filledBoxMutex);
                                if (box->boxType == gmBoxType::BT_BUY)
                                {
                                    hasUnFilledBuyBox = false;
                                    filledBuyBoxes.push_back(box);
                                }
                                else
                                {
                                    hasUnFilledSellBox = false;
                                    filledSellBoxes.push_back(box);
                                }

                                // Notify the box square off thread that a box has been filled
                                boxFilledCond.notify_one();
                            }
                        }
                        else
                        {
                            // Got complete fill for a leg of a incomplete box
                            // Need to squreaoff the leg asap to remove this leg and eventually box
                            Price orderPrice = 0;
                            {
                                // Get the current price for the contract
                                std::lock_guard<std::mutex> contractGaurd(contractMutex);
                                orderPrice = orderInfo->contract->book.price[orderInfo->side];

                                // Remove order info from the contract
                                orderInfo->contract->orders.erase(orderInfo->linkId);
                            }

                            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Got complete fill for incomplete box, need to place square off order boxid: %d, linkId: %d, orderId: %d, contractid:%d", box->boxId, orderInfo->linkId, orderFill.orderId, orderFill.contractId));

                            cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                               { placeSquareOffOrder(
                                                                     {contract, orderPrice, 
                                                                      (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                      fillQty}); });

                            // Set the leg order as null, reason removing its order info
                            box->legs[orderInfo->legId] = nullptr;

                            // Remove orderinfo object for the leg from collection and return to object pool for reuse.
                            orderIdToOrderInfo.erase(orderInfoItr);
                            orderInfoObjects.returnObject(orderInfo);
                        }
                    }
                    else
                    {
                        // It's a squareoff order, so no box associated with the order
                        // Remove orderInfo from orders collection that is completely filled,
                        // and return orderInfo object for reuse.

                        // TODO: remove this log line after testing
                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Order fill for squareoff order id: %d, linkid: %d, Contract id: %d, fill quantity: %d, fill price: %d", orderFill.orderId, orderInfo->linkId, orderFill.contractId, orderFill.fillQuantity, orderFill.fillPrice));

                        {
                            // Remove order info from the contract
                            std::lock_guard<std::mutex> contractGaurd(contractMutex);
                            orderInfo->contract->orders.erase(orderInfo->linkId);
                        }

                        orderIdToOrderInfo.erase(orderInfoItr);
                        orderInfoObjects.returnObject(orderInfo);
                    }
                }
                break;
                case gmOrderStatus::OS_PARTIAL_FILL:
                {
                    // Partial fill for a incomplete box
                    if (orderInfo->box && gmBoxState::BS_INCOMPLETE == orderInfo->box->state)
                    {
                        // Get the associated box with the order
                        Box *box = orderInfo->box;

                        // Need to squreaoff the leg for fill quantity asap to remove this leg and eventually box
                        Price orderPrice = 0;
                        {
                            // Get the current price for the contract
                            std::lock_guard<std::mutex> contractGaurd(contractMutex);
                            orderPrice = orderInfo->contract->book.price[orderInfo->side];

                            // Remove order info from the contract
                            orderInfo->contract->orders.erase(orderInfo->linkId);
                        }

                        cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                           { placeSquareOffOrder(
                                                                 {contract, orderPrice,
                                                                  (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                  fillQty}); });

                        // Then, place cancel order to cancel the remaining quantity
                        cancelSquareoffOrderTaskQueue.push([orderId = orderFill.orderId, this]()
                                                           { placeCancelOrder(orderId); });
                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Got partial fill for the incomplete box, placing cancel order for the remaining quantity boxid: %d, orderId: %d, contractid:%d", box->boxId, orderFill.orderId, orderFill.contractId));
                    }
                }
                break;
            }


        }

        /**
         * Get called when received market tick data from exchange
         * 
         * @param mdTick Market data tick that called it for
         */
        void gmMDCallback(const gmMDTick &mdTick) override
        {
            // Update contract's price if tick is for the contract.
            {
                std::lock_guard<std::mutex> guard(contractMutex);
                std::unordered_map<int, Contract*>::iterator contractItr = contractIdToContract.find(mdTick.contractId);
                if (contractItr != contractIdToContract.end())
                {
                    // Set top sell price
                    if (mdTick.askSide[0].quantity)
                    {
                        contractItr->second->book.price[gmBuyOrSell::BS_SELL] = mdTick.askSide[0].price;
                    }

                    // Set top buy price
                    if (mdTick.bidSide[0].quantity)
                    {
                        contractItr->second->book.price[gmBuyOrSell::BS_BUY] = mdTick.bidSide[0].price;
                    }

                    {
                        // Let modify thread know that orders need to send price modify request
                        std::lock_guard contractOrderGaurd(contractOrdersMutex);
                        contractOrderInfoMap = &contractItr->second->orders;
                        doModify.notify_one();
                    }
                }
            }

            // If has any buy box waiting to get filled, then don't place further buy box order
            const auto liveBuySpread = (!hasUnFilledBuyBox) ? getLiveBuySpread() : 0;
            if (liveBuySpread && liveBuySpread > boxStrategyParams.requiredBuyEntrySpread_)
            {
                // TODO:: remove this log line after testing
               ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Going to place Buy box order: %d > %d", liveBuySpread, boxStrategyParams.requiredBuyEntrySpread_));

               // Mark that buy box order going to place and is not filled yet
               hasUnFilledBuyBox = true;

               // Add box order in order task queue for buy box order placement
               boxorderTaskQueue.push([&, this]()
                                    { placeBoxOrder({gmBoxType::BT_BUY, liveBuySpread,
                                    {&lowerCallContract, lowerCallContract.book.price[gmBuyOrSell::BS_SELL], gmBuyOrSell::BS_BUY},
                                    {&lowerPutContract, lowerPutContract.book.price[gmBuyOrSell::BS_BUY], gmBuyOrSell::BS_SELL},
                                    {&higherCallContract, higherCallContract.book.price[gmBuyOrSell::BS_BUY], gmBuyOrSell::BS_SELL},
                                    {&higherPutContract, higherPutContract.book.price[gmBuyOrSell::BS_SELL], gmBuyOrSell::BS_BUY}});
                                    });
            }

            // If has any sell box waiting to get filled, then don't place further sell box order
            const auto liveSellSpread = (!hasUnFilledSellBox) ? getLiveSellSpread() : 0;
            if (liveSellSpread && liveSellSpread > boxStrategyParams.requiredSellEntrySpread_)
            {
                // TODO:: remove this log line after testing
              ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Going to place sell box order: %d > %d", liveSellSpread, boxStrategyParams.requiredSellEntrySpread_));

              // Mark that sell box order going to place and is not filled yet
              hasUnFilledSellBox = true;

              // Add box order in order task queue for sell box order placement
              boxorderTaskQueue.push([&, this]()
                                   { placeBoxOrder({gmBoxType::BT_SELL, liveSellSpread,
                                   {&lowerCallContract, lowerCallContract.book.price[gmBuyOrSell::BS_BUY], gmBuyOrSell::BS_SELL},
                                   {&lowerPutContract, lowerPutContract.book.price[gmBuyOrSell::BS_SELL], gmBuyOrSell::BS_BUY},
                                   {&higherCallContract, higherCallContract.book.price[gmBuyOrSell::BS_SELL], gmBuyOrSell::BS_BUY},
                                   {&higherPutContract, higherPutContract.book.price[gmBuyOrSell::BS_BUY], gmBuyOrSell::BS_SELL}});
                                   });
            }

            // TODO: remove after testing
            if (tickCount == UINT16_MAX)
            {
                tickCount = 0;
            }
            ++tickCount;
            
        }

        /**
         * Stop the box strategy
         * 
         * This will signals other thread to stop and place squareoff order if require.
         */
        void stop()
        {
            // Stopping the box strategy
            strategyStopped = true;
            noBoxOrder = true;

            ::logger->writeLog(gmLogLevel::INFO, "Stopping the strategy");

            // Join the remove square off box thread
            if (removeSquareOffBoxThread.joinable())
            {
                // Notify the thread to wake up from waiting
                boxFilledCond.notify_one();
                removeSquareOffBoxThread.join();
            }

            // Join the incomplete box thread
            if (removeIncompleteBoxThread.joinable())
            {
                // Notify the thread to wake up from waiting
                incompleteBoxAddedCond.notify_one();
                removeIncompleteBoxThread.join();
            }

            // Day closed can not place square off or cancel orders now
            if (dayClosed)
            {
                ::logger->writeLog(gmLogLevel::INFO, "Day closed can not place square off or cancel orders");
                return;
            }

            OrderInfo *orderInfo = nullptr;

            int lastLinkId = linkId;

            // We are giving some time (about 5 seconds) to square off or cancel any pending orders in the system.

            std::this_thread::sleep_for(std::chrono::seconds(5));
            {
                std::lock_guard<std::mutex> guard(orderInfoMutex);

                // TODO:: remove this log line after testing
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Total Orders count: %d ", orderIdToOrderInfo.size()));

                // Iterate through orderInfo collection to place squareoff order or cancel
                for (auto orderInfoItr = orderIdToOrderInfo.begin(); orderInfoItr != orderIdToOrderInfo.end();)
                {
                    // Get order info object
                    orderInfo = orderInfoItr->second;

                    if (orderInfo->linkId > lastLinkId)
                    {
                        ++orderInfoItr;
                        continue;
                    }

                    switch (orderInfo->orderStatus)
                    {
                        case gmOrderStatus::OS_COMPLETE_FILL:
                        {
                            // TODO:: remove this log line after testing
                            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Stop - complete fill order, placing squareoff order for order id: %d, linkid: %d, contract id:%d, qty: %d, side(was|now):%d|%d", orderInfo->orderId, orderInfo->linkId, orderInfo->contract->id, orderInfo->fillQty, orderInfo->side, (gmBuyOrSell::BS_BUY == orderInfo->side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY));

                            // Remove orderinfo from contract
                            Price orderPrice = 0;
                            {
                                std::lock_guard<std::mutex> guard(contractMutex);
                                orderPrice = orderInfo->contract->book.price[orderInfo->side];
                                orderInfo->contract->orders.erase(orderInfo->linkId);
                            }

                            // Add square off order task
                            cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                               { placeSquareOffOrder(
                                                                     {contract,
                                                                      orderPrice,
                                                                      (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                      fillQty}); });

                            // Remove order info object for the leg
                            orderInfoItr = orderIdToOrderInfo.erase(orderInfoItr);
                            orderInfoObjects.returnObject(orderInfo);
                        }
                        break;
                        case gmOrderStatus::OS_PARTIAL_FILL:
                        {
                            // TODO:: remove this log line after testing
                            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Stop - partial fill order, placing squareoff order for order id: %d, contract id:%d, qty: %d, side(was|now):%d|%d", orderInfo->orderId, orderInfo->contract->id, orderInfo->fillQty, orderInfo->side, (gmBuyOrSell::BS_BUY == orderInfo->side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY));

                            Price orderPrice = 0;
                            {
                                std::lock_guard<std::mutex> guard(contractMutex);
                                orderPrice = orderInfo->contract->book.price[orderInfo->side];
                            }

                            // Add square off order task for filled quantity
                            cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                               { placeSquareOffOrder(
                                                                     {contract,
                                                                      orderPrice,
                                                                      (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                      fillQty}); });
                        }
                        // Fallthrough to cancel the remaining quantity in case of partial order
                        case gmOrderStatus::OS_ACCEPTED:
                            // TODO:: remove this log line after testing
                            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Stop - accepted order, placing cancel order for order id: %d, linkId: %d", orderInfo->orderId, orderInfo->linkId));
                            // Add cancel task to cancel the remaining quantity for partial and accepted order.
                            cancelSquareoffOrderTaskQueue.push([orderId = orderInfo->orderId, this]()
                                                               { placeCancelOrder(orderId); });

                            // Move to next element
                            ++orderInfoItr;
                            break;
                        default:
                            // Move to next element
                            ++orderInfoItr;
                            break;
                    }
                }
            }

            // Give 5 seconds to let complete the tasks, if any
            std::this_thread::sleep_for(std::chrono::seconds(5));

            // Join the box order thread
            if (boxOrderThread.joinable())
            {
                // Notify the thread to wake up from waiting
                boxorderTaskQueue.push([this](){stopTaskThread();});
                boxOrderThread.join();
            }

            // Forcing that day is closed, so that modify thread and cancelSquareoffOrderThread
            // will stop working further and can be joined.
            dayClosed = true;

            // Join the cancelSquareoffOrderThread thread
            if (cancelSquareoffOrderThread.joinable())
            {
                // Notify the thread to wake up from waiting
                cancelSquareoffOrderTaskQueue.push([this]()
                                                   { stopTaskThread(); });
                cancelSquareoffOrderThread.join();
            }

            // Join the modify order thread
            if (modifyOrderThread.joinable())
            {
                doModify.notify_one();
                modifyOrderThread.join();
            }
        }

        private:

        /**
         * Thread picks an new box order task from the box order queue and run it.
         * 
         * It picks task from  queue as soon as task get available and executes it.
         */
        void runBoxOrderTask()
        {
            while (true)
            {
                // Pick a new box order task from the queue
                std::function<void()> orderTask;
                if (boxorderTaskQueue.pop(orderTask))
                {
                    // Not allowed to place further box order, as strategy is going to stop now.
                    if (strategyStopped)
                    {
                        ::logger->writeLog(gmLogLevel::INFO, "Strategy is going to stop, can not place further box orders.");
                        return;
                    }

                    // Not allowed to place further box order, this usually happend when current time reached
                    // the specified time after which no further box orders are allowed to place.
                    if (noBoxOrder)
                    {
                        ::logger->writeLog(gmLogLevel::INFO, "No further box orders are allowed to place, reached todays time limit for box orders.");
                        return;
                    }

                    // Execute order task
                    orderTask();
                }
            }
        }

        /**
         * Thread picks an cancel or squareoff order task from the its order queue and run it.
         * 
         * It picks task from  queue as soon as task get available and executes it.
         */
        void runCancelSquareoffOrderTask()
        {
            while (true)
            {
                // Pick a cancel or squareoff order task from the queue
                std::function<void()> orderTask;
                if (cancelSquareoffOrderTaskQueue.pop(orderTask))
                {
                    // If day closed, can not place further orders now.
                    if (dayClosed)
                    {
                        ::logger->writeLog(gmLogLevel::INFO, "No further cancel or squareoff orders are allowed to place, either day closed or strategy is stopping.");
                        return;
                    }

                    // Execute order task
                    orderTask();
                }
            }
        }

        /**
         * Modify order thread, run at every specified modify interval
         * and modify the order's price with aggresive price to
         * make the order get filled asap at the top price.
         */
        void modifyOrder()
        {
            // Contract's orders
            std:unordered_map<int, OrderInfo*> *contractOrdersMap;

            while (!dayClosed)
            {
                // Put modify thread in sleep for sometime, this avoid unneccassary burden on the system
                // to modify the order on every price change which get notified on every tick.
                std::this_thread::sleep_for(std::chrono::microseconds(modifyinterval));

                // Wait for contract's orders collection that needs price modification
                {
                    std::unique_lock lock(contractOrdersMutex);
                    doModify.wait(lock, [this]()
                                                 { return dayClosed || (contractOrderInfoMap && !contractOrderInfoMap->empty()); /*&& needModification;*/ });

                    // Day closed to make further modify orders, return from this thread
                    if (dayClosed)
                    {
                        return;
                    }
                    // Get contract's orderInfo collection to work on
                    contractOrdersMap = contractOrderInfoMap;
                    
                    // Allows not to process on the old orders
                    contractOrderInfoMap = nullptr;
                }

                // Still no orders to process, bypass the loop and again wait for the orders
                if (!contractOrdersMap)
                {
                    continue;
                }

                // iterate over each order and modify its price with aggresive price.
                std::lock_guard<std::mutex> lock(orderInfoMutex);
                for (auto &orderInfo : *contractOrdersMap)
                {
                    //::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Modify thread [%lu] checking each orders", std::this_thread::get_id()));
                    // If order is accepted and not fully filled then modfiy its price
                    if (orderInfo.second->orderStatus == gmOrderStatus::OS_ACCEPTED || orderInfo.second->orderStatus == gmOrderStatus::OS_PARTIAL_FILL)
                    {
                        std::lock_guard<std::mutex> guard(contractMutex);
                        std::unordered_map<int, Contract *>::iterator contractItr = contractIdToContract.find(orderInfo.second->contract->id);
                        if (contractItr == contractIdToContract.end())
                        {
                            ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("Modify - Contract ID not found: %d", orderInfo.second->contract->id));
                            continue;
                        }

                        if (orderInfo.second->side == gmBuyOrSell::BS_BUY)
                        {
                            if (orderInfo.second->price != contractItr->second->book.price[gmBuyOrSell::BS_SELL])
                            {
                                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Modify - Sell for Contract id: %d, side: %d, old Price: %d, new price: %d", orderInfo.second->contract->id, (int)orderInfo.second->side, orderInfo.second->price, contractItr->second->book.price[gmBuyOrSell::BS_SELL]));
                                // Modify the order with aggresive ask price
                                gmTCPModifyQuoteInfo modify(orderInfo.second->orderId, gmOrderType::OT_LIMIT, gmOrderValidity::OV_DAY, 0 /*stoploss_price*/,
                                                            contractItr->second->book.price[gmBuyOrSell::BS_SELL] * GM_BOX_PRICE_MULTIPLIER, orderInfo.second->qty);

                                gmClientApi::placeOrder(modify);
                                orderInfo.second->price = contractItr->second->book.price[gmBuyOrSell::BS_SELL];
                            }
                        }
                        else if (orderInfo.second->side == gmBuyOrSell::BS_SELL)
                        {
                            if (orderInfo.second->price != contractItr->second->book.price[gmBuyOrSell::BS_BUY])
                            {
                                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Modify - Buy for Contract id: %d, side: %d, old Price: %d, new price: %d", orderInfo.second->contract->id, (int)orderInfo.second->side, orderInfo.second->price, contractItr->second->book.price[gmBuyOrSell::BS_BUY]));
                                // Modify the order with aggresive bid price
                                gmTCPModifyQuoteInfo modify(orderInfo.second->orderId, gmOrderType::OT_LIMIT, gmOrderValidity::OV_DAY, 0 /*stoploss_price*/,
                                                            contractItr->second->book.price[gmBuyOrSell::BS_BUY] * GM_BOX_PRICE_MULTIPLIER, orderInfo.second->qty);
                                gmClientApi::placeOrder(modify);
                                orderInfo.second->price = contractItr->second->book.price[gmBuyOrSell::BS_BUY];
                            }
                        }
                    }
                }

                // Processed the contract's orderInfo, make it ready to have new set of orders
                contractOrdersMap = nullptr;
            }
        }

        /**
         * This thread removes the squareoffed box when has any squareoff boxes.
         */
        void removeSquareOffBox()
        {
            uint squaredOffBoxCount = 0; // Number of squared off boxes
            uint filledBuyBoxCount = 0;  // Number of buy boxes filled
            uint filledSellBoxCount = 0; // Number of sell boxes filled

            // Keep running untill box strategy not stopped
            while (!strategyStopped)
            {
                std::unique_lock<std::mutex> lock(filledBoxMutex);
                boxFilledCond.wait(lock, [this]()
                                   { return strategyStopped || (!filledBuyBoxes.empty() && !filledSellBoxes.empty()); });

                // Get number of elements filled in each box type
                filledBuyBoxCount = filledBuyBoxes.size();
                filledSellBoxCount = filledSellBoxes.size();

                // Either box has no items, skip and again wait for items.
                if (!filledBuyBoxCount && !filledSellBoxCount)
                {
                    continue;
                }

                // Number of boxex that has been squared off, least common box count from each type of boxes
                squaredOffBoxCount = (filledBuyBoxCount <= filledSellBoxCount) ? filledBuyBoxCount : filledSellBoxCount;

                // Iterate over each box to look for filled buy and sell box and remove them.
                for (uint idx = 0; idx < squaredOffBoxCount; ++idx)
                {
                    {
                        std::lock_guard<std::mutex> guardOrderInfo(orderInfoMutex);

                        // Go over each legs of the box and remove its orderinfo object
                        for (uint legIdx = 0; legIdx < MAX_LEG_IN_BOX; ++legIdx)
                        {
                            {
                                // Remove orderinfo from contract
                                std::lock_guard<std::mutex> guard(contractMutex);
                                filledBuyBoxes[idx]->legs[legIdx]->contract->orders.erase(filledBuyBoxes[idx]->legs[legIdx]->linkId);
                                filledSellBoxes[idx]->legs[legIdx]->contract->orders.erase(filledSellBoxes[idx]->legs[legIdx]->linkId);
                            }

                            orderIdToOrderInfo.erase(filledBuyBoxes[idx]->legs[legIdx]->orderId);
                            orderInfoObjects.returnObject(filledBuyBoxes[idx]->legs[legIdx]);

                            orderIdToOrderInfo.erase(filledSellBoxes[idx]->legs[legIdx]->orderId);
                            orderInfoObjects.returnObject(filledSellBoxes[idx]->legs[legIdx]);
                        }
                    }

                    // Remove the boxes from filled buy and sell box collection
                    filledBuyBoxes.erase(filledBuyBoxes.begin() + idx);
                    filledSellBoxes.erase(filledSellBoxes.begin() + idx);

                    // Return the box object for reuse.
                    boxObjects.returnObject(filledBuyBoxes[idx]);
                    boxObjects.returnObject(filledSellBoxes[idx]);
                }
            }
        }

        /**
         * This thread removes incomplete box from the system.
         * 
         * Incomplete box whose all leg's orders are not placed successfully or
         * if any of its leg order got rejected by the exchange making the box incomplete.
         */
        void removeIncompleteBox()
        {
            Box *box = nullptr;
            OrderInfo *orderInfo = nullptr;

            // Keep running untill box strategy not stopped
            while (!strategyStopped)
            {
                // Wait to have any incomplete box
                {
                    std::unique_lock<std::mutex> lock(incompleteBoxesMutex);
                    incompleteBoxAddedCond.wait(lock, [this]()
                                                { return strategyStopped || !incompleteBoxes.empty(); });
                }

                // Wait for a second to get the order in the system
                std::this_thread::sleep_for(std::chrono::seconds(1));

                {
                    std::lock_guard<std::mutex> lock(incompleteBoxesMutex);

                    // Iterate over each incomplete box in the system and try to remove it from the system
                    for (auto boxItr = incompleteBoxes.begin(); boxItr != incompleteBoxes.end();)
                    {
                        // Get the box object
                        box = *boxItr;

                        uint idx = 0;
                        {
                            // Iterate over each valid legs of the box
                            while (box->legs[idx])
                            {
                                // Get order info object for a leg
                                orderInfo = box->legs[idx];

                                switch (orderInfo->orderStatus)
                                {
                                    case gmOrderStatus::OS_COMPLETE_FILL:
                                    {

                                        // TODO:: remove this log line after testing
                                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Incomplete box - complete fill order, placing squareoff order for order id: %d, linkid: %d, leg: %d, contract id:%d, qty: %d, side(was|now):%d|%d", orderInfo->orderId, orderInfo->linkId, idx + 1, orderInfo->contract->id, orderInfo->fillQty, orderInfo->side, (gmBuyOrSell::BS_BUY == orderInfo->side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY));

                                        // Remove orderinfo from contract
                                        Price orderPrice = 0;
                                        {
                                            std::lock_guard<std::mutex> guard(contractMutex);
                                            orderPrice = orderInfo->contract->book.price[orderInfo->side];
                                            orderInfo->contract->orders.erase(orderInfo->linkId);
                                        }

                                        cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                                           { placeSquareOffOrder(
                                                                                 {contract,
                                                                                  orderPrice,
                                                                                  (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                                  fillQty}); });

                                        {
                                            // Remove order info object for the leg
                                            std::lock_guard<std::mutex> orderInfoLock(orderInfoMutex);
                                            orderIdToOrderInfo.erase(orderInfo->orderId);
                                            orderInfoObjects.returnObject(orderInfo);
                                        }
                                    }
                                    break;
                                    case gmOrderStatus::OS_PARTIAL_FILL:
                                    {
                                        // TODO:: remove this log line after testing
                                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Incomelete box - partial fill order, placing squareoff order for order id: %d, leg: %d, contract id:%d, qty: %d, side(was|now):%d|%d", orderInfo->orderId, idx + 1, orderInfo->contract->id, orderInfo->fillQty, orderInfo->side, (gmBuyOrSell::BS_BUY == orderInfo->side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY));

                                        // Add square off order task for filled quantity
                                        Price orderPrice = 0;
                                        {
                                            std::lock_guard<std::mutex> guard(contractMutex);
                                            orderPrice = orderInfo->contract->book.price[orderInfo->side];
                                        }

                                        cancelSquareoffOrderTaskQueue.push([contract = orderInfo->contract, side = orderInfo->side, fillQty = orderInfo->fillQty, orderPrice, this]()
                                                                           { placeSquareOffOrder(
                                                                                 {contract,
                                                                                  orderPrice,
                                                                                  (gmBuyOrSell::BS_BUY == side) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY,
                                                                                  fillQty}); });
                                    }
                                    // Fallthrough to cancel the remaining quantity in case of partial order
                                    case gmOrderStatus::OS_ACCEPTED:
                                        // TODO:: remove this log line after testing
                                        ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Incomelete box - accepted order, placing cancel order for order id: %d, leg: %d", orderInfo->orderId, idx + 1));

                                        // Add cancel task to cancel the remaining quantity for partial and accepted order.
                                        cancelSquareoffOrderTaskQueue.push([orderId = orderInfo->orderId, this]()
                                                                           { placeCancelOrder(orderId); });
                                    break;
                                    default:
                                    break;
                                }

                                // Mark that now leg has no order info
                                box->legs[idx] = nullptr;

                                // Move to next leg
                                ++idx;
                            }
                        }

                        // Erase the box and return the box object for reuse.
                        boxItr = incompleteBoxes.erase(boxItr);
                        boxObjects.returnObject(box);
                    }
                }
            }
        }

        /**
         * Place box order.
         * 
         * It places box's all 4 legs order as per the received box order details.
         * 
         * @param boxOrder  Details for placing box order.
         */
        void placeBoxOrder(const BoxOrder boxOrder)
        {
            // Get box id for this box
            uint boxNo = ++boxId;

            // When a leg order is failed to place, then it helps to identify to
            // remove the orderInfo of pending leg orders from the collections.
            // And also helps to know how many leg orders placed in a box.
            uint legOrderCount = 0;

            // Get the box object for this box order
            Box *box = boxObjects.getObject();

            // Get orderInfo objects for 4 legs of the box
            OrderInfo *orderInfoLeg[MAX_LEG_IN_BOX] = {orderInfoObjects.getObject(), orderInfoObjects.getObject(), orderInfoObjects.getObject(), orderInfoObjects.getObject()};

            // Update leg 1 order info object with leg 1 order details
            orderInfoLeg[0]->orderId = 0;
            orderInfoLeg[0]->linkId = ++linkId;
            orderInfoLeg[0]->contract = boxOrder.leg1.contract;
            orderInfoLeg[0]->legId = 0;
            orderInfoLeg[0]->qty = boxStrategyParams.requiredBuyEntryQty_;
            orderInfoLeg[0]->price = boxOrder.leg1.price;
            orderInfoLeg[0]->side = boxOrder.leg1.side;
            orderInfoLeg[0]->fillQty = 0;
            orderInfoLeg[0]->orderStatus = gmOrderStatus::OS_SENT_NEW;
            orderInfoLeg[0]->box = box;

            // Update leg 2 order info object with leg 2 order details
            orderInfoLeg[1]->orderId = 0;
            orderInfoLeg[1]->linkId = ++linkId;
            orderInfoLeg[1]->contract = boxOrder.leg2.contract;
            orderInfoLeg[1]->legId = 1;
            orderInfoLeg[1]->qty = boxStrategyParams.requiredBuyEntryQty_;
            orderInfoLeg[1]->price = boxOrder.leg2.price;
            orderInfoLeg[1]->side = boxOrder.leg2.side;
            orderInfoLeg[1]->fillQty = 0;
            orderInfoLeg[1]->orderStatus = gmOrderStatus::OS_SENT_NEW;
            orderInfoLeg[1]->box = box;

            // Update leg 3 order info object with leg 3 order details
            orderInfoLeg[2]->orderId = 0;
            orderInfoLeg[2]->linkId = ++linkId;
            orderInfoLeg[2]->contract = boxOrder.leg3.contract;
            orderInfoLeg[2]->legId = 2;
            orderInfoLeg[2]->qty = boxStrategyParams.requiredBuyEntryQty_;
            orderInfoLeg[2]->price = boxOrder.leg3.price;
            orderInfoLeg[2]->side = boxOrder.leg3.side;
            orderInfoLeg[2]->fillQty = 0;
            orderInfoLeg[2]->orderStatus = gmOrderStatus::OS_SENT_NEW;
            orderInfoLeg[2]->box = box;

            // Update leg 4 order info object with leg 4 order details
            orderInfoLeg[3]->orderId = 0;
            orderInfoLeg[3]->linkId = ++linkId;
            orderInfoLeg[3]->contract = boxOrder.leg4.contract;
            orderInfoLeg[3]->legId = 3;
            orderInfoLeg[3]->qty = boxStrategyParams.requiredBuyEntryQty_;
            orderInfoLeg[3]->price = boxOrder.leg4.price;
            orderInfoLeg[3]->side = boxOrder.leg4.side;
            orderInfoLeg[3]->fillQty = 0;
            orderInfoLeg[3]->orderStatus = gmOrderStatus::OS_SENT_NEW;
            orderInfoLeg[3]->box = box;

            // Update box with all 4 legs details
            box->boxId = boxNo;
            box->boxType = boxOrder.boxType;
            box->legFillCount = 0;
            box->legs[0] = orderInfoLeg[0];
            box->legs[1] = orderInfoLeg[1];
            box->legs[2] = orderInfoLeg[2];
            box->legs[3] = orderInfoLeg[3];

            // Each contracts has its order list
            {
                std::lock_guard<std::mutex> guard(contractMutex);
                orderInfoLeg[0]->contract->orders.emplace(orderInfoLeg[0]->linkId, orderInfoLeg[0]);
                orderInfoLeg[1]->contract->orders.emplace(orderInfoLeg[1]->linkId, orderInfoLeg[1]);
                orderInfoLeg[2]->contract->orders.emplace(orderInfoLeg[2]->linkId, orderInfoLeg[2]);
                orderInfoLeg[3]->contract->orders.emplace(orderInfoLeg[3]->linkId, orderInfoLeg[3]);
            }

            // Store all 4 leg's OrderInfo object to order info collections before placing orders.
            // The idea is to free the orderinfo collection so that orders updates won't need to wait
            // to get the orderinfo collection and then miss its update.
            {
                std::lock_guard<std::mutex> guard(orderInfoMutex);
                orderIdToOrderInfo.emplace(orderInfoLeg[0]->linkId, orderInfoLeg[0]);
                orderIdToOrderInfo.emplace(orderInfoLeg[1]->linkId, orderInfoLeg[1]);
                orderIdToOrderInfo.emplace(orderInfoLeg[2]->linkId, orderInfoLeg[2]);
                orderIdToOrderInfo.emplace(orderInfoLeg[3]->linkId, orderInfoLeg[3]);
            }

            // Place leg 1 order
            gmTCPNewQuoteInfo quote = gmTCPNewQuoteInfo(0, boxStrategyParams.strategyId_, boxStrategyParams.clientId_, boxOrder.leg1.contract->id,
                                           gmOrderType::OT_LIMIT, gmOrderValidity::OV_DAY, boxOrder.leg1.side,
                                           0 /*stoplossPrice*/, boxOrder.leg1.price * GM_BOX_PRICE_MULTIPLIER, boxStrategyParams.requiredBuyEntryQty_, orderInfoLeg[0]->linkId,
                                           0 /*connectionId*/, boxStrategyParams.dealerId_, gmOrderSubType::OST_NONE);

            auto retVal = gmClientApi::placeOrder(quote);
            if (gmAPIReturn::GM_RETURN_SUCCESS != retVal)
            {
                box->state = gmBoxState::BS_INCOMPLETE;
                // Failed to place first leg order, no need to place other legs order, incomplete box.
                ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("Failed to place 1st leg order with error code:%d, can not place remaining legs orders", retVal));
                goto removeLegOrderInfo;
            }

            // Place leg 2 order
            quote.newQuoteInfo.contractId = boxOrder.leg2.contract->id;
            quote.newQuoteInfo.price = boxOrder.leg2.price * GM_BOX_PRICE_MULTIPLIER;
            quote.newQuoteInfo.buySell = boxOrder.leg2.side;
            quote.newQuoteInfo.linkId = orderInfoLeg[1]->linkId;

            retVal = gmClientApi::placeOrder(quote);
            if (gmAPIReturn::GM_RETURN_SUCCESS != retVal)
            {
                box->state = gmBoxState::BS_INCOMPLETE;
                // Failed to place second leg order, no need to place other legs order, incomplete box.
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Failed to place 2nd leg order with error code:%d, can not place remaining legs orders", retVal));

                // Remove pending 3 orderinfo objects from collection
                legOrderCount = 1;
                goto removeLegOrderInfo;
            }

            // Place leg 3 order
            quote.newQuoteInfo.contractId = boxOrder.leg3.contract->id;
            quote.newQuoteInfo.price = boxOrder.leg3.price * GM_BOX_PRICE_MULTIPLIER;
            quote.newQuoteInfo.buySell = boxOrder.leg3.side;
            quote.newQuoteInfo.linkId = orderInfoLeg[2]->linkId;

            retVal = gmClientApi::placeOrder(quote);
            if (gmAPIReturn::GM_RETURN_SUCCESS != retVal)
            {
                box->state = gmBoxState::BS_INCOMPLETE;
                // Failed to place third leg order, no need to place other legs order, incomplete box.
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Failed to place 3rd leg order with error code:%d, can not place remaining legs orders", retVal));

                // Remove pending 2 orderinfo objects from collection
                legOrderCount = 2;
                goto removeLegOrderInfo;
            }

            // Place 4th leg order
            quote.newQuoteInfo.contractId = boxOrder.leg4.contract->id;
            quote.newQuoteInfo.price = boxOrder.leg4.price * GM_BOX_PRICE_MULTIPLIER;
            quote.newQuoteInfo.buySell = boxOrder.leg4.side;
            quote.newQuoteInfo.linkId = orderInfoLeg[3]->linkId;

            retVal = gmClientApi::placeOrder(quote);
            if (gmAPIReturn::GM_RETURN_SUCCESS != retVal)
            {
                box->state = gmBoxState::BS_INCOMPLETE;
                // Failed to place fourth leg order, no need to place other legs order, incomplete box.
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Failed to place 4th leg order with error code:%d", retVal));

                // Remove pending 1 orderinfo objects from collection
                legOrderCount = 3;
                goto removeLegOrderInfo;
            }

            // TODO: remove this lig line aftertesting
            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Box order placed for box: %d", boxNo));

            // All work done for the box.
            return;

            // Got error in placing leg orders for the box
            removeLegOrderInfo:
            {
                // TODO: remove log line after testing
                ::logger->writeLog(gmLogLevel::INFO, "Failed to place leg orders, so undoing orderInfo object and box.");

                // As failed to place all leg order sucessfully for the box, we can't declare it a box 
                (box->boxType == gmBoxType::BT_BUY) ? hasUnFilledBuyBox = false : hasUnFilledSellBox = false;

                // Remove orders from the contracts
                {
                    std::lock_guard<std::mutex> guard(contractMutex);
                    for (uint i = legOrderCount; i < MAX_LEG_IN_BOX; ++i)
                    {
                        orderInfoLeg[i]->contract->orders.erase(orderInfoLeg[i]->linkId);
                    }
                }

                // Remove the leg order's info object from the orderInfo collection
                // and return the orderInfo object for reuse for which order not yet placed
                {
                    std::lock_guard<std::mutex> guard(orderInfoMutex);
                    for (uint i = legOrderCount; i < MAX_LEG_IN_BOX; ++i)
                    {
                        box->legs[i] = nullptr;
                        orderIdToOrderInfo.erase(orderInfoLeg[i]->linkId);
                        orderInfoObjects.returnObject(orderInfoLeg[i]);
                    }
                }

                // Add the box to incomplete collection and notify
                std::lock_guard<std::mutex> incompleteBoxesGaurd(incompleteBoxesMutex);
                incompleteBoxes.emplace(box);
                incompleteBoxAddedCond.notify_one();
            }
        }

        /**
         * Place square off order
         */
        void placeSquareOffOrder(const Order order)
        {
            // Get orderInfo objects for order to place
            OrderInfo *orderInfo = orderInfoObjects.getObject();
            orderInfo->orderId = 0;
            orderInfo->linkId = ++linkId;
            orderInfo->contract = order.contract;
            orderInfo->legId = 0;
            orderInfo->qty = order.qty;
            orderInfo->price = order.price;
            orderInfo->side = order.side;
            orderInfo->fillQty = 0;
            orderInfo->orderStatus = gmOrderStatus::OS_SENT_NEW;
            orderInfo->box = nullptr;

            // Add order to its contract
            {
                std::lock_guard<std::mutex> guard(contractMutex);
                orderInfo->contract->orders.emplace(orderInfo->linkId, orderInfo);
            }

            // Store OrderInfo object to order info collections before placing orders.
            // The idea is to free the orderinfo collection so that orders updates won't need to wait
            // to get the orderinfo collection and then miss its update.
            {
                std::lock_guard<std::mutex> guard(orderInfoMutex);
                orderIdToOrderInfo.emplace(orderInfo->linkId, orderInfo);
            }

            // Place order
            gmTCPNewQuoteInfo quote = gmTCPNewQuoteInfo(0, boxStrategyParams.strategyId_, boxStrategyParams.clientId_, orderInfo->contract->id,
                                           gmOrderType::OT_LIMIT, gmOrderValidity::OV_DAY, orderInfo->side,
                                           0 /*stoplossPrice*/, orderInfo->price * GM_BOX_PRICE_MULTIPLIER, orderInfo->qty, orderInfo->linkId,
                                           0 /*connectionId*/, boxStrategyParams.dealerId_, gmOrderSubType::OST_NONE);

            auto retVal = gmClientApi::placeOrder(quote);
            if (gmAPIReturn::GM_RETURN_SUCCESS != retVal)
            {
                // Failed to place square off order.
                ::logger->writeLog(gmLogLevel::ERROR, gmStringHelper::formattedString("Failed to place square off order with error code:%d", retVal));

                goto error;
            }

           // TODO: remove this lig line aftertesting
            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Squareoff order placed for contract: %d, linkid: %d, price: %d", quote.newQuoteInfo.contractId, orderInfo->linkId, quote.newQuoteInfo.price));

            return;

            error:
                // Remove order from the contract
                {
                    std::lock_guard<std::mutex> guard(contractMutex);
                    orderInfo->contract->orders.erase(orderInfo->linkId);
                }

                // Remove order info from orderInfo collection
                {
                    std::lock_guard<std::mutex> guard(orderInfoMutex);
                    orderIdToOrderInfo.erase(orderInfo->linkId);
                }

                // Return orderInfo object
                orderInfoObjects.returnObject(orderInfo);
        }

        /**
         * Place cancel order to cancel a order identified by orderId.
         * 
         * @param orderId Order id of a order to cancel
         */
        void placeCancelOrder(const int orderId)
        {
            gmTCPCancelQuoteInfo cancelQuoteInfo(orderId);
            placeOrder(cancelQuoteInfo);
            //TODO: remove this log line after testing
            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Cancel order placed for order id: %d",orderId));
        }

        /**
         * Create boxes and order info objects from previous positions if any.
         * 
         * This enquire the database to get positions against this strategy
         * and create box and order info if any position left to squareoffed.
         */
        void createBoxesFromPreviousPositions()
        {
            std::lock_guard<std::mutex> orderInfoGaurd(orderInfoMutex);
            std::lock_guard<std::mutex> contractIdMutex(contractMutex);
            std::lock_guard<std::mutex> filledBuySellBoxMutex(filledBoxMutex);

            gmConfigurationMaster *configMaster = gmConfigurationMaster::GetInstance();

            // Get current date
            std::string date = gmStringHelper::getCurrentDateYYYYMMDD();

            // Get previous positions
            std::unique_ptr<SACommand> resultSet = configMaster->GetPositionFromDB(date, "NSE");
            if (!resultSet)
            {
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("No previous positions received from DB for date: %s", date));
                return;
            }

            // Call securities, refer securityType table in DB
            std::set<int> callSecurities{2, 6, 12, 31, 33};

            // Local variables
            std::string securityId;
            int lowStrikeprice = INT_MAX;
            int highStrikeprice = INT_MIN;
            uint lowStrikepriceCallPos = 0;
            uint highStrikepriceCallPos = 0;
            uint8_t entries = 0;

            // Store position for later use in the code
            std::vector<Position> previousPositions;

            if (resultSet->RowsAffected())
            {
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Got previous positions creating box/order from them: %d", resultSet->RowsAffected()));
            }

            // Iterate over the results (positions)
            while (resultSet->FetchNext())
            {
                Position pos;
                pos.algoId = resultSet->Field(_TSA("algoid")).asLong();
                pos.clientId = resultSet->Field(_TSA("accountid")).asLong();
                pos.dealerId = resultSet->Field(_TSA("dealerid")).asLong();
                pos.buyQty = resultSet->Field(_TSA("buy")).asLong();
                pos.sellQty = resultSet->Field(_TSA("sell")).asLong();
                securityId = resultSet->Field(_TSA("securityid")).asString().GetMultiByteChars();
                pos.tradeDate = resultSet->Field(_TSA("tradedate")).asString().GetMultiByteChars();
                pos.contractId = resultSet->Field(_TSA("contractid")).asLong();

                // Identifiy security type: call or put
                std::set<int>::const_iterator itr = callSecurities.find(resultSet->Field(_TSA("securitytype")).asLong());
                pos.isCall = (itr == callSecurities.end()) ? false : true;

                // Get price from security id
                // SecurityId is made of following items: 12 char ticker + 3 char exchange + 2 security type + 8 expiry date + then price,
                // So price starts from 25th position till null character.
                pos.price = stoi(securityId.substr(25, securityId.size()));

                // Get the low strike price
                if (pos.price <= lowStrikeprice)
                {
                    lowStrikeprice = pos.price;
                    if (pos.isCall)
                    {
                        // Store position's location in the vector
                        // if its a call's low strike price for faster access
                        lowStrikepriceCallPos = entries;
                    }
                }

                // Get the high strike price
                if (pos.price >= highStrikeprice)
                {
                    highStrikeprice = pos.price;
                    if (pos.isCall)
                    {
                        // Store position's location in the vector
                        // if its a call's high strike price for faster access
                        highStrikepriceCallPos = entries;
                    }
                }

                // Add the position in the positions vector for later use
                previousPositions.push_back(pos);

                // Got all leg positions for a box
                if (MAX_LEG_IN_BOX == ++entries)
                {
                    // Calculate no. of boxes required to store previou positions.
                    // This is because for same set of contracts their entries are clubbed.
                    Qty orderQty = (previousPositions[0].buyQty == 0) ? previousPositions[0].sellQty : previousPositions[0].buyQty;
                    uint noOfBoxes = orderQty / boxStrategyParams.requiredBuyEntryQty_;
                    bool createBox(false);

                    // Orderinfo and box to store positions
                    OrderInfo *orderInfoLeg[MAX_LEG_IN_BOX] = {nullptr};
                    Box *box = nullptr;

                    // Previous position and current box strategy's strike price are same,
                    // means box strategy is running for same strike price, box can be squared off.
                    // That's why creating box for the previous positions.
                    if (boxStrategyParams.strikePrice == lowStrikeprice)
                    {
                        createBox = true;
                    }

                    for (uint idx = 0; idx < noOfBoxes; ++idx)
                    {
                        // Create order info object for each leg
                        for (uint legNo = 0; legNo < MAX_LEG_IN_BOX; ++legNo)
                        {
                            // Get orderInfo objects for 4 legs of the box
                            orderInfoLeg[legNo] = orderInfoObjects.getObject();

                            // Update each leg order info object from previous position
                            orderInfoLeg[legNo]->orderId = orderInfoLeg[legNo]->linkId = ++linkId;
                            orderInfoLeg[legNo]->legId = legNo;
                            // Assuming the quantity is not chaning in DB on per day run
                            orderInfoLeg[legNo]->qty = orderInfoLeg[legNo]->fillQty = boxStrategyParams.requiredBuyEntryQty_;
                            orderInfoLeg[legNo]->price = 0;
                            orderInfoLeg[legNo]->side = (previousPositions[legNo].buyQty == 0) ? gmBuyOrSell::BS_SELL : gmBuyOrSell::BS_BUY;
                            orderInfoLeg[legNo]->orderStatus = gmOrderStatus::OS_COMPLETE_FILL;
                            orderInfoLeg[legNo]->box = nullptr;

                            // If using same contracts then get it from the collection, else create new contracts.
                            auto contractItr = contractIdToContract.find(previousPositions[legNo].contractId);
                            if (contractItr != contractIdToContract.end())
                            {
                                orderInfoLeg[legNo]->contract = contractItr->second;
                                contractItr->second->orders.emplace(orderInfoLeg[legNo]->linkId, orderInfoLeg[legNo]);
                            }
                            else
                            {
                                Contract *contract = new Contract(previousPositions[legNo].contractId);
                                orderInfoLeg[legNo]->contract = contract;
                                contractIdToContract.emplace(contract->id, contract);
                                contract->orders.emplace(orderInfoLeg[legNo]->linkId, orderInfoLeg[legNo]);
                            }

                            // Add leg's order info object into order info collection
                            orderIdToOrderInfo.emplace(orderInfoLeg[legNo]->linkId, orderInfoLeg[legNo]);
                        }

                        // Do we need to creare a box for its leg orders
                        if (createBox)
                        {
                            box = boxObjects.getObject();
                            box->boxId = ++boxId;

                            // Being a position, box must be added to buy or sell filled box collection to squareoff
                            if (previousPositions[lowStrikepriceCallPos].buyQty)
                            {
                                box->boxType = gmBoxType::BT_BUY;
                                filledBuyBoxes.push_back(box);
                            }
                            else
                            {
                                box->boxType = gmBoxType::BT_SELL;
                                filledSellBoxes.push_back(box);
                            }
                            box->legFillCount = MAX_LEG_IN_BOX;
                            box->state = gmBoxState::BS_COMPLETE;

                            for (uint legCount = 0; legCount < MAX_LEG_IN_BOX; ++legCount)
                            {
                                // Set box in each leg order info object
                                orderInfoLeg[legCount]->box = box;

                                // Store leg order info object in its box
                                box->legs[legCount] = orderInfoLeg[legCount];
                            }
                        }
                    }

                    // Remove the positions as they are worked on
                    previousPositions.clear();
                    entries = 0;
                }
            }
            
            // Log total boxes and orders created from previous positions
            if (resultSet->RowsAffected())
            {
                ::logger->writeLog(gmLogLevel::INFO, "Details of boxes and orders created from previous positions.");
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Total boxes created: %d", filledBuyBoxes.size() + filledSellBoxes.size()));
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Total buy filled boxes created: %d", filledBuyBoxes.size()));
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Total sell filled boxes created: %d", filledSellBoxes.size()));
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Total orders created: %d", orderIdToOrderInfo.size()));
            }
        }

        /**
         * Thread controls the activity of placing orders by allowing them 
         * when to place orders and when not.
         * 
         * Thread informs the system when day is closed (at market closed time)
         * and when reached at specified time to disallow to place new box order.
         */
        void controlActivity()
        {
            int64_t noBoxOrderDuration;
            int64_t dayClosedDuration;

            {
                int64_t noBoxOrderTimeInSeconds;
                int64_t dayClosedTimeInSeconds;
                int64_t currentTimeInSeconds;
                int dayClosedHours = 15;
                int dayClosedMinutes = 30;

                // Convert no box order time in seconds
                noBoxOrderTimeInSeconds = ((boxStrategyParams.noBoxOrderAfterHour / 100) * 3600) + ((boxStrategyParams.noBoxOrderAfterHour % 100) * 60);

                // Convert day closed time in seconds
                dayClosedTimeInSeconds = (dayClosedHours * 3600) + (dayClosedMinutes * 60);

                // Get current time
                auto currentTime = std::time(nullptr);

                // Set time as per day closed time
                std::tm currentTimeTM = *std::localtime(&currentTime);
                currentTimeInSeconds = (currentTimeTM.tm_hour * 3600) + (currentTimeTM.tm_min * 60);

                // Calculate duration left to reach no box order time from current time
                noBoxOrderDuration = noBoxOrderTimeInSeconds - currentTimeInSeconds;

                // Calculate duration left to reach day closed time from current time
                dayClosedDuration = dayClosedTimeInSeconds - currentTimeInSeconds;
            }

            // Here thread first going to sleep for no box order duration and then for day closed duartion,
            // because day closed is always comes after no box order.

            // If time left to reach the no box order duration, then sleep the thread for that duration
            if (noBoxOrderDuration > 0)
            {
                // Sleep for the thread for no box duration
                std::this_thread::sleep_for(std::chrono::seconds(noBoxOrderDuration));
            }

            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Reached at time of day when further box orders can not be placed."));
            
            // Reached the no box order time, let inform the system that no further box order now.
            noBoxOrder = true;

            // If time left to reach the day closed duration, then sleep the thread for that duration
            if (dayClosedDuration > 0)
            {
                // Sleep for the thread for day closed duration
                std::this_thread::sleep_for(std::chrono::seconds(dayClosedDuration));
            }

            ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("Reached at time of day closed, can not place any orders now."));

            // Reached the day closed time, let inform the system that no further order as day is closed now.
            dayClosed = true;
        }

        /**
         * Calculate live buy spread.
         * 
         * @return Live buy spread.
         */
        inline Price getLiveBuySpread() const
        {
            // lowPutPrice + highCallPrice - lowCallPrice - highPutPrice + boxStrategyParams.strikeDiff_;
            if (!lowerPutContract.book.price[gmBuyOrSell::BS_BUY] || !higherCallContract.book.price[gmBuyOrSell::BS_BUY] || !lowerCallContract.book.price[gmBuyOrSell::BS_SELL] || !higherPutContract.book.price[gmBuyOrSell::BS_SELL])
            {
                return 0;
            }

            auto val = lowerPutContract.book.price[gmBuyOrSell::BS_BUY] + higherCallContract.book.price[gmBuyOrSell::BS_BUY]
                     - lowerCallContract.book.price[gmBuyOrSell::BS_SELL] - higherPutContract.book.price[gmBuyOrSell::BS_SELL]
                     + boxStrategyParams.strikeDiff_;

            // TODO:: remove this log line after testing and directly have return at above line
            if (tickCount == UINT16_MAX)
            {
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("GetLiveBuySpread:  %d = LP_BID(%d): %d + HC_BID(%d): %d - LC_ASK(%d): %d - HP_ASK(%d): %d + StrikeDiff: %d", val,
                                                                                     lowerPutContract.id,
                                                                                     lowerPutContract.book.price[gmBuyOrSell::BS_BUY],
                                                                                     higherCallContract.id,
                                                                                     higherCallContract.book.price[gmBuyOrSell::BS_BUY],
                                                                                     lowerCallContract.id,
                                                                                     lowerCallContract.book.price[gmBuyOrSell::BS_SELL],
                                                                                     higherPutContract.id,
                                                                                     higherPutContract.book.price[gmBuyOrSell::BS_SELL],
                                                                                     boxStrategyParams.strikeDiff_));
            }

            return val;
        }

        /**
         * Calculate live sell spread.
         * 
         * @return Live sell spread.
         */
        inline Price getLiveSellSpread() const
        {
            // highPutPrice + lowCallPrice - lowPutPrice - highCallPrice - boxStrategyParams.strikeDiff_;
            if(!higherPutContract.book.price[gmBuyOrSell::BS_BUY] || !lowerCallContract.book.price[gmBuyOrSell::BS_BUY] || !lowerPutContract.book.price[gmBuyOrSell::BS_SELL] || !higherCallContract.book.price[gmBuyOrSell::BS_SELL])
            {
                return 0;
            }
         
            auto val = higherPutContract.book.price[gmBuyOrSell::BS_BUY] + lowerCallContract.book.price[gmBuyOrSell::BS_BUY]
                     - lowerPutContract.book.price[gmBuyOrSell::BS_SELL] - higherCallContract.book.price[gmBuyOrSell::BS_SELL]
                     - boxStrategyParams.strikeDiff_;

            // TODO:: remove this log line after testing and directly have return at above line
            if (tickCount == UINT16_MAX)
            {
                ::logger->writeLog(gmLogLevel::INFO, gmStringHelper::formattedString("GetLiveSellSpread: % d = HP_BID(%d): %d + LC_BID(%d): %d - LP_ASK(%d): %d - HC_ASK(%d): %d - StrikeDiff: %d", val,
                                                                                      higherPutContract.id,
                                                                                      higherPutContract.book.price[gmBuyOrSell::BS_BUY],
                                                                                      lowerCallContract.id,
                                                                                      lowerCallContract.book.price[gmBuyOrSell::BS_BUY],
                                                                                      lowerPutContract.id,
                                                                                      lowerPutContract.book.price[gmBuyOrSell::BS_SELL],
                                                                                      higherCallContract.id,
                                                                                      higherCallContract.book.price[gmBuyOrSell::BS_SELL],
                                                                                      boxStrategyParams.strikeDiff_));
            }
            return val;
        }

        /**
         * This is a dummy task to stop threads which are waiting on task queue.
         * 
         * On stop this dummy task is added into task queue that
         * wakeups the thread and thread then able to identify that stratey is stopped
         * then thread stops themself.
         */
        void stopTaskThread()
        {
            return;
        }


        // Contracts for box strategy
        Contract lowerCallContract, lowerPutContract, higherCallContract, higherPutContract;

        // Box strategy parameters
        BoxStrategyParams boxStrategyParams;

        // True when box strategy is stopped, and other threads get to know by this.
        std::atomic<bool> strategyStopped;

        // Thread controls when to stop placing further box orders or any type of orders.
        std::thread controlActivityThread;

        // True when current time reached specified time in DB after which no new box orders can be placed.
        std::atomic<bool> noBoxOrder;

        // True when current time reached to day closed time, then no orders can be placed.
        std::atomic<bool> dayClosed;

        // Incremental box id, assigned to box to identify a box uniquely in the system.
        std::atomic<uint> boxId = 0;

        // Incremental link id, assigned to orderInfo, to identify the orders internally. 
        int linkId = 0;

        // OrderInfo object pool, allow to get orderInfo object when need and return back after use.
        ObjectPool<OrderInfo> orderInfoObjects;

        // Box object pool, allow to get box object when need and return back after use.
        ObjectPool<Box> boxObjects;

        // True when strategy has unfilled buy box in the system,
        // allows to control not to place further buy type box order till box get filled.
        std::atomic<bool> hasUnFilledBuyBox;

        // True when strategy has unfilled sell box in the system,
        // allows to control not to place further sell type box order till box get filled.
        std::atomic<bool> hasUnFilledSellBox;

        // Map to hold contract against its contract id
        std::unordered_map<int /*contractId*/, Contract*> contractIdToContract;

        // Provide mutual access on contractIdToContract.
        std::mutex contractMutex;

        // Map holds orderInfo objects against order id, it is used to track an order in the system.
        std::unordered_map<int /*orderId*/, OrderInfo*> orderIdToOrderInfo;

        // Provide mutual access on orderIdToOrderInfo.
        std::mutex orderInfoMutex;

        // Task queue for managing a box order task for box order thread.
        ThreadSafeQueue<std::function<void()>> boxorderTaskQueue;

        // Thread that place a box order by taking a task from boxorderTaskQueue.
        std::thread boxOrderThread;

        // Task queue for managing a cancel or squareoff order task for cancel and squareoff thread.
        ThreadSafeQueue<std::function<void()>> cancelSquareoffOrderTaskQueue;

        // Thread that place a cancel or squareoff order by taking a task from cancelSquareoffOrderTaskQueue.
        std::thread cancelSquareoffOrderThread;

        // Pointer to a map of orderInfo object of a contract,
        // that needs to send modify order upon contract's price update.
        std::unordered_map<int, OrderInfo*> *contractOrderInfoMap;

        // Provide mutual access on contractOrderInfoMap.
        std::mutex contractOrdersMutex;

        // Thread send a modify order on contract's orders when contract's price get updated.
        // Thread tries to keep the order on aggressive price so it will be filled soon.
        std::thread modifyOrderThread;

        // Condition variable on which modify thread keeping waiting until contract's price get updated.
        std::condition_variable doModify;

        // Interval in microseconds to sleep the modify thread, this put modify thread in rest for sometime.
        uint64_t modifyinterval;

        // Collection of buy type complete filled boxes.
        std::vector<Box*> filledBuyBoxes;

        // Collection of sell type complete filled boxes.
        std::vector<Box*> filledSellBoxes;

        // Provide mutual access on filledBuyBoxes and filledSellBoxes.
        std::mutex filledBoxMutex;

        // Thread removes squared off boxes from the system.
        std::thread removeSquareOffBoxThread;

        // Condition variable on which thread removes squared off boxes.
        std::condition_variable boxFilledCond;

        // Set of incomplete (not all 4 legs orders are successfully placed) boxes.
        std::unordered_set<Box*> incompleteBoxes;

        // Provide mutual access on incompleteBoxes.
        std::mutex incompleteBoxesMutex;

        // Thread removes incomplete boxes from the system.
        std::thread removeIncompleteBoxThread;

        // Condition variable no which remove incomplete thread removes incomplete boxes.
        std::condition_variable incompleteBoxAddedCond;
    };
}

