/* Struct-by-value function return (hidden-pointer ABI) - the pattern
 * kernel/lang/script/eval.c's mkint()/mkstr() constructors rely on
 * throughout the scripting language's evaluator. */
typedef struct {
    int type;
    int val;
} tagged_t;

static tagged_t make(int type, int val) {
    tagged_t t;
    t.type = type;
    t.val = val;
    return t;
}

static tagged_t identity(tagged_t t) {
    return t;
}

int main(void) {
    tagged_t a = make(1, 10);
    tagged_t b = make(2, 20);
    tagged_t c = identity(a);
    int sum = a.type + a.val + b.type + b.val + c.type + c.val;
    return sum; /* (1+10)+(2+20)+(1+10) = 11+22+11 = 44 */
}
