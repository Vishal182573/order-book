#pragma once // Yeh compiler ko batata hai ki is file ko sirf ek baar include karo (taaki multiple definition errors na aaye)

// ─── Slippage Estimator ───────────────────────────────────────────────────────
// Market order place karte waqt price aur actual execute hone wale price mein
// jo difference aata hai, usko slippage kehte hain. Yeh usko estimate karta hai.
// Isko Smart Order Router (SOR) ya Risk Engine use karte hain trade limit check karne ke liye.
/* Maan lijiye aap screen par dekhte hain ki Bitcoin ka price $50,000 chal raha hai. Aap ek bohot bada "Market Buy Order" lagate hain 100 Bitcoins ka. Lekin problem yeh hai ki $50,000 par sirf 10 Bitcoin bechne wale baithe hain. Uske baad wale log $50,010 pe 20 Bitcoin, aur uske baad wale $50,050 pe 70 Bitcoin bech rahe hain.

Jab aapka order execute hoga, toh woh Order Book ko "khaata" jayega (eat through the book):

10 BTC mila $50,000 pe (Best Price)
20 BTC mila $50,010 pe
70 BTC mila $50,050 pe
Toh aapka Average Buying Price kareeb $50,038 aayega. Aapne screen par expect kiya tha $50,000, lekin actual mein average mila $50,038. Yeh jo $38 ka loss per coin hua (price ka "fisal" jana order ke size ki wajah se), isko trading ki bhasha mein Slippage kehte hain. Bada order = Badi Slippage. */

#include "order_book.hpp" // Order book (snaphot, price levels) laane ke liye header.

namespace obk { // 'obk' namespace banaya taaki functions iske andar safely rahein

// 'struct' ek custom data type hai jismein related variables store hote hain.
// Yeh struct estimation result ko hold karta hai.
struct SlippageEstimate {
    Price    expected_avg_price;  // Hamein on-average kis rate par saari quantity milegi.
    Price    worst_price;         // Market depth (order book) clear karte hue sabse kharab rate kya mila.
    Price    slippage_cost;       // (avg_price - ref_price) * qty: Total kitna paisa extra lag gaya slippage ki wajah se.
    double   slippage_bps;        // Slippage 'basis points' mein (0.01% = 1 bps). Finance mein common calculation.
    Quantity fillable_qty;        // Book mein itni liquidity hai ki actual mein kitni quantity execute ho sakti hai.
    bool     fully_fillable;      // Kya hamari saari order qty fill ho payegi ya book empty ho jayegi?
};

// 'class' SlippageEstimator banaya hai jo algorithms ko wrap karta hai.
class SlippageEstimator {
public: 
    // 'static' keyword batata hai ki is function ko call karne ke liye object banane ki zarurat nahi.
    // '[[nodiscard]]' compile hone pe warning dega agar call karne wale ne iska result ignore/waste kiya.
    // 'noexcept' compiler se wada karta hai ki yeh function koi exception (error) throw nahi karega (taaki speed fast ho).
    [[nodiscard]] static SlippageEstimate estimate(
        const OrderBookSnapshot& snap, // 'const' and '&' (reference): snapshot ko modify nahi karenge, aur copy bhi nahi banayenge (fast operation).
        Side      side,                // Hamein buy karna hai ya sell karna hai.
        Quantity  order_qty,           // Hamein kitni total quantity kharidni ya bechni hai.
        Price     ref_price            // Benchmark price (mid-price ya last_trade_price) jiske against hum slippage dekhenge.
    ) noexcept {
        SlippageEstimate est{}; // Naya empty result struct banaya jise end mein return karenge.
        
        // Agar main BUY side hoon, toh main ASKS (sellers) ko hit karunga. Agar main SELL hoon, toh BIDS (buyers) ko.
        const auto& levels = (side == Side::Buy) ? snap.asks : snap.bids;

        Quantity remaining  = order_qty; // Kitni aur qty fill karna baaki hai usko track karne ke liye.
        Price    cost       = 0;         // Total notional volume (price * qty) track karega.
        // Buy ke time worst price max jayegi, toh start mein 0. Sell ke time min jayegi, toh aage ke liye infinity jaisa 'max()' limit liya hai.
        Price    worst      = (side == Side::Buy) ? 0 : std::numeric_limits<Price>::max();

        // Range-based for loop: Books 'levels' vector ek ek line read karega (top of book se leke deep tak).
        for (const auto& lvl : levels) {
            if (remaining == 0) break; // Agar hamari required order quantity puri ho gayi, toh loop ruk jayega.
            
            // Is price level pe hamein kitna mil sakta hai minimum: ya toh bachhi hui saari, ya is level pe jitni padi hai vo saari.
            Quantity fill = std::min(remaining, lvl.qty);
            
            // Total cost badhate hain (Paisa = kitna_khareeda * price) [QTY_SCALE decimal points control karne ke liye use hota hai]
            cost      += fill * lvl.price / QTY_SCALE;
            
            remaining -= fill; // Bachi hui required quantity kam ho gayi.
            
            // Update karo Worst price kya mili hame. (Buy mein sabse mehnga, Sell mein sabse sasta)
            worst      = (side == Side::Buy) ? std::max(worst, lvl.price)
                                             : std::min(worst, lvl.price);
        }

        Quantity filled = order_qty - remaining; // Total kitni quantity mil gayi order book se.
        est.fillable_qty      = filled;
        est.fully_fillable    = (remaining == 0); // Agar remaining 0 hai, mtlb order book se sara mil gaya.
        est.worst_price       = worst;
        // Expected Avg Price = Total Kharcha / Total Mily Hui Quantity
        est.expected_avg_price= (filled > 0) ? (cost * QTY_SCALE / filled) : 0;
        
        // Slippage cost calculate karte waqt: Buy mein hum ref price se kitna MAHNGAA (expensive) kharide aur Sell mein kitna SASTA.
        est.slippage_cost     = (side == Side::Buy)
            ? (est.expected_avg_price - ref_price) * filled / QTY_SCALE
            : (ref_price - est.expected_avg_price) * filled / QTY_SCALE;
            
        // Slippage BPS mein (Basis Points). Total percentage slippage * 10000. 10000 isliye kyuki 1% = 100 bps
        est.slippage_bps      = (ref_price > 0)
            ? static_cast<double>(est.slippage_cost) / (ref_price * filled / QTY_SCALE) * 10000.0
            : 0.0;
            
        return est; // Result struct lautaa diya function ne.
    }
};

} // namespace obk
