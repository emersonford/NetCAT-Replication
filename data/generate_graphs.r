#!/usr/bin/env Rscript
require(docopt)
library(ggplot2)
library(reshape2)
'Usage:
   generate_graphs.R <filename> [--lxlim=<ns>] [--rxlim=<ns>] [--lthres=<ns>] [--rthres=<ns>] [--diffqthres=<q>] [--positivediff] [--displaymean] [--samplebar]

Options:
   --lxlim=<ns>      Left limit of x-axis on histogram [default: 0]
   --rxlim=<ns>      Right limit of x-axis on histogram [default: 10000]
   --lthres=<ns>     Data to filter out if less than [default: 0]
   --rthres=<ns>     Data to filter out if greater than [default: 100000]
   --positivediff    Filter negative diffs
   --diffqthres=<q>  Data to filter out if diff is greater than quantile(q) (0 <= q <= 1) [default: 1]
   --displaymean     Display mean lines in histograms
   --samplebar       Sample data before generating bar chart to reduce generation time

 ]' -> doc

opts <- docopt(doc)
opts$lxlim <- as.numeric(opts$lxlim)
opts$rxlim <- as.numeric(opts$rxlim)
opts$lthres <- as.numeric(opts$lthres)
opts$rthres <- as.numeric(opts$rthres)
opts$diffqthres <- as.numeric(opts$diffqthres)
# print(opts)

first.read.col <- 3
second.read.col <- 4

datatype <- "nanoseconds"

data <- read.csv(opts$filename)

df.orig <- data.frame(first_read=data[,first.read.col], second_read=data[,second.read.col])
df.orig$id <- seq_len(nrow(df.orig))
df.orig$diff <- df.orig$first_read - df.orig$second_read

df <- subset(df.orig, first_read > opts$lthres & second_read > opts$lthres)
df <- subset(df, first_read < opts$rthres & second_read < opts$rthres)
if (opts$positivediff) {
    df <- subset(df, diff > 0)
}
df <- subset(df, diff <= quantile(first_read - second_read, opts$diffqthres))

print(paste("Filtered", nrow(df.orig) - nrow(df), "rows."), sep=" ")

histogram = ggplot(melt(df[,c("first_read", "second_read")]), aes(x = value, fill = variable)) +
    geom_density(alpha=0.5) +
    xlim(opts$lxlim, opts$rxlim) +
    labs(title=opts$filename, x=datatype, fill="read type", caption=paste(names(opts[1:7]), opts[1:7], sep = "=", collapse=" ")) +
    theme(plot.caption=element_text(size=4))

if (opts$displaymean) {
    histogram + geom_vline(xintercept=mean(df$first_read), linetype="dashed", color="firebrick1") +
        geom_vline(xintercept=mean(df$second_read), linetype="dashed", color="cyan4")
}

ggsave(paste(strsplit(opts$filename, "\\.")[[1]][1], "-histogram.png", sep=""))


if (opts$samplebar) {
    # Sample the data first so the barchart doesn't take forever.
    s <- sample(df$id, 100000)
    df <- subset(df, df$id %in% s)
}

# print(head(df))

barchart = ggplot(data=df , aes(x = id, y = diff)) +
    geom_bar(stat="identity") +
#    scale_fill_gradient2(low="red", high="green", mid="black") +
    labs(title=opts$filename, x="read-write-read iteration", y=paste(datatype, " diff", sep=""), caption=paste(names(opts[1:7]), opts[1:7], sep = "=", collapse=" ")) +
    theme(plot.caption=element_text(size=4))

ggsave(paste(strsplit(opts$filename, "\\.")[[1]][1], "-barchart.png", sep=""))
