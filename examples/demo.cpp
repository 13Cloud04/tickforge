// Tiny interactive book. Commands (one per line):
//   buy  <id> <price> <qty>      sell <id> <price> <qty>
//   mbuy <id> <qty>              msell <id> <qty>
//   cancel <id>                  book
#include <iostream>
#include <string>
#include <vector>

#include "tickforge/order_book.hpp"

using namespace tf;

struct Printer {
    void on_trade(const Trade& t) {
        std::cout << "  TRADE " << t.qty << " @ " << t.price << "  (maker " << t.maker
                  << ", taker " << t.taker << ")\n";
    }
};

int main() {
    Printer p;
    OrderBook<Printer> book(p);
    std::string cmd;
    while (std::cin >> cmd) {
        OrderId id;
        Price px;
        Qty q;
        if (cmd == "buy" || cmd == "sell") {
            std::cin >> id >> px >> q;
            book.add_limit(id, cmd == "buy" ? Side::Buy : Side::Sell, px, q);
        } else if (cmd == "mbuy" || cmd == "msell") {
            std::cin >> id >> q;
            book.add_market(id, cmd == "mbuy" ? Side::Buy : Side::Sell, q);
        } else if (cmd == "cancel") {
            std::cin >> id;
            std::cout << (book.cancel(id) ? "  cancelled\n" : "  unknown order\n");
        } else if (cmd == "book") {
            std::vector<LevelView> v;
            book.snapshot(Side::Sell, v, 5);
            for (auto it = v.rbegin(); it != v.rend(); ++it)
                std::cout << "        " << it->price << "  x " << it->qty << "  (" << it->orders << ")\n";
            std::cout << "  ------\n";
            book.snapshot(Side::Buy, v, 5);
            for (const auto& l : v)
                std::cout << "  " << l.price << "        x " << l.qty << "  (" << l.orders << ")\n";
        }
    }
}
